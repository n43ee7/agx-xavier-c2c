// gpu_check - plain CUDA program (runtime API + cuBLAS). Checks a GPU gets the
// right answers and measures latency/bandwidth. Doesn't know about C2C, under
// `c2c-gpu run` the remote GPU is just one more device.
//
//   gpu_check [-d DEV]   checks on one device (default 0)
//   gpu_check --all      all devices: one saxpy split over every GPU, then
//                        the per-device checks on each one
// prints one JSON object, exit 0 only if everything passed
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <sys/time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
    fprintf(stderr, "CUDA %s: %s (line %d)\n", #x, cudaGetErrorString(e_), __LINE__); exit(2); } } while (0)
#define CB(x) do { cublasStatus_t s_ = (x); if (s_ != CUBLAS_STATUS_SUCCESS) { \
    fprintf(stderr, "cuBLAS %s: %d\n", #x, (int)s_); exit(2); } } while (0)

static double now() { timeval t; gettimeofday(&t, nullptr); return t.tv_sec + t.tv_usec * 1e-6; }

__device__ float g_scale;          // set via cudaMemcpyToSymbol
__device__ unsigned long long g_count;

__global__ void saxpy(float a, const float* x, float* y, size_t n) {
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
        y[i] = a * x[i] * g_scale + y[i];
}
__global__ void sum_ints(const unsigned* v, size_t n) {
    unsigned long long s = 0;
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) s += v[i];
    atomicAdd(&g_count, s);
}
__global__ void empty_kernel() {}
struct Pair { int a; double b; char c; float3 v; };   // passed by value
__global__ void struct_param(Pair p, double* out) { *out = p.a + p.b + p.c + p.v.x + p.v.y + p.v.z; }

static unsigned rng_state = 12345;
static unsigned rnd() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

struct Result { bool ok = true; std::string json; };

static void add(std::string& j, const char* k, double v, int prec = 3) {
    char b[96]; snprintf(b, sizeof b, "%s\"%s\":%.*f", j.size() > 1 ? "," : "", k, prec, v); j += b;
}
static void addb(std::string& j, const char* k, bool v) {
    char b[96]; snprintf(b, sizeof b, "%s\"%s\":%s", j.size() > 1 ? "," : "", k, v ? "true" : "false"); j += b;
}

static Result check_device(int dev) {
    Result R; std::string& j = R.json; j = "{";
    CK(cudaSetDevice(dev));
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, dev));
    char b[256]; snprintf(b, sizeof b, "\"device\":%d,\"name\":\"%s\",\"pci_domain\":%d", dev, p.name, p.pciDomainID); j += b;

    // 1. saxpy w/ a __device__ symbol, 64 Mi floats
    const size_t n = 64u << 20;
    std::vector<float> x(n), y(n), ref(n);
    for (size_t i = 0; i < n; i++) { x[i] = (rnd() % 2000) / 100.f - 10; y[i] = (rnd() % 2000) / 100.f; }
    float scale = 0.5f, a = 3.f;
    for (size_t i = 0; i < n; i++) ref[i] = a * x[i] * scale + y[i];
    float *dx, *dy;
    CK(cudaMalloc(&dx, n * 4)); CK(cudaMalloc(&dy, n * 4));
    CK(cudaMemcpyToSymbol(g_scale, &scale, sizeof scale));
    double t0 = now();
    CK(cudaMemcpy(dx, x.data(), n * 4, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dy, y.data(), n * 4, cudaMemcpyHostToDevice));
    double t1 = now();
    saxpy<<<p.multiProcessorCount * 16, 256>>>(a, dx, dy, n);
    CK(cudaGetLastError());
    CK(cudaDeviceSynchronize());
    double t2 = now();
    std::vector<float> out(n);
    CK(cudaMemcpy(out.data(), dy, n * 4, cudaMemcpyDeviceToHost));
    double t3 = now();
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) if (out[i] != ref[i]) bad++;
    addb(j, "saxpy_ok", bad == 0); R.ok &= bad == 0;
    add(j, "htod_pageable_mbs", 2.0 * n * 4 / (t1 - t0) / 1e6, 1);
    add(j, "dtoh_pageable_mbs", 1.0 * n * 4 / (t3 - t2) / 1e6, 1);

    // 2. atomic reduction into a __device__ counter
    std::vector<unsigned> v(n);
    unsigned long long cpu = 0;
    for (size_t i = 0; i < n; i++) { v[i] = rnd() & 0xffff; cpu += v[i]; }
    unsigned* dv; CK(cudaMalloc(&dv, n * 4));
    CK(cudaMemcpy(dv, v.data(), n * 4, cudaMemcpyHostToDevice));
    unsigned long long zero = 0, gpu = 0;
    CK(cudaMemcpyToSymbol(g_count, &zero, sizeof zero));
    sum_ints<<<p.multiProcessorCount * 16, 256>>>(dv, n);
    CK(cudaMemcpyFromSymbol(&gpu, g_count, sizeof gpu));
    addb(j, "reduction_ok", gpu == cpu); R.ok &= gpu == cpu;

    // 3. cuBLAS SGEMM 512^3 vs CPU reference
    const int N = 512;
    std::vector<float> A(N * N), B(N * N), C(N * N), Cr(N * N, 0.f);
    for (auto& e : A) e = (rnd() % 200) / 100.f - 1;
    for (auto& e : B) e = (rnd() % 200) / 100.f - 1;
    for (int c = 0; c < N; c++) for (int k = 0; k < N; k++) {
        float bk = B[c * N + k];
        for (int r = 0; r < N; r++) Cr[c * N + r] += A[k * N + r] * bk;
    }
    float *dA, *dB, *dC;
    CK(cudaMalloc(&dA, N * N * 4)); CK(cudaMalloc(&dB, N * N * 4)); CK(cudaMalloc(&dC, N * N * 4));
    CK(cudaMemcpy(dA, A.data(), N * N * 4, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dB, B.data(), N * N * 4, cudaMemcpyHostToDevice));
    cublasHandle_t h; CB(cublasCreate(&h));
    float one = 1, zf = 0;
    CB(cublasSgemm(h, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &one, dA, N, dB, N, &zf, dC, N));
    CK(cudaMemcpy(C.data(), dC, N * N * 4, cudaMemcpyDeviceToHost));
    double maxerr = 0;
    for (int i = 0; i < N * N; i++) maxerr = std::max(maxerr, (double)fabs(C[i] - Cr[i]));
    addb(j, "sgemm_ok", maxerr < 1e-2); R.ok &= maxerr < 1e-2;
    add(j, "sgemm_max_abs_err", maxerr, 6);
    CB(cublasDestroy(h));

    // 4. struct by value
    Pair pr{3, 0.25, 2, make_float3(1.f, 2.f, 4.f)};
    double* dd; CK(cudaMalloc(&dd, 8));
    struct_param<<<1, 1>>>(pr, dd);
    double got = 0; CK(cudaMemcpy(&got, dd, 8, cudaMemcpyDeviceToHost));
    addb(j, "struct_param_ok", got == 12.25); R.ok &= got == 12.25;

    // 5. 4 streams + pinned mem + events, chunked pipeline
    const int NS = 4; const size_t chunk = 4u << 20;
    float* hp; CK(cudaMallocHost(&hp, NS * chunk * 4));
    for (size_t i = 0; i < NS * chunk; i++) hp[i] = float(i % 1000);
    float* dz; CK(cudaMalloc(&dz, NS * chunk * 4));
    cudaStream_t st[NS]; cudaEvent_t ev[NS];
    for (int s = 0; s < NS; s++) { CK(cudaStreamCreate(&st[s])); CK(cudaEventCreate(&ev[s])); }
    float one_scale = 1.f; CK(cudaMemcpyToSymbol(g_scale, &one_scale, 4));
    for (int s = 0; s < NS; s++) {
        CK(cudaMemcpyAsync(dz + s * chunk, hp + s * chunk, chunk * 4, cudaMemcpyHostToDevice, st[s]));
        saxpy<<<64, 256, 0, st[s]>>>(1.f, dz + s * chunk, dz + s * chunk, chunk);   // y = 2y
        CK(cudaMemcpyAsync(hp + s * chunk, dz + s * chunk, chunk * 4, cudaMemcpyDeviceToHost, st[s]));
        CK(cudaEventRecord(ev[s], st[s]));
    }
    for (int s = 0; s < NS; s++) CK(cudaEventSynchronize(ev[s]));
    size_t sbad = 0;
    for (size_t i = 0; i < NS * chunk; i++) if (hp[i] != 2.f * float(i % 1000)) sbad++;
    addb(j, "streams_ok", sbad == 0); R.ok &= sbad == 0;

    // 6. pinned bandwidth, 256 MiB each direction
    const size_t bw = 256u << 20;
    void *hb, *db; CK(cudaMallocHost(&hb, bw)); CK(cudaMalloc(&db, bw));
    memset(hb, 1, bw);
    CK(cudaMemcpy(db, hb, bw, cudaMemcpyHostToDevice));
    t0 = now(); CK(cudaMemcpy(db, hb, bw, cudaMemcpyHostToDevice)); t1 = now();
    CK(cudaMemcpy(hb, db, bw, cudaMemcpyDeviceToHost)); t2 = now();
    add(j, "htod_pinned_mbs", bw / (t1 - t0) / 1e6, 1);
    add(j, "dtoh_pinned_mbs", bw / (t2 - t1) / 1e6, 1);

    // 7. latencies
    empty_kernel<<<1, 1>>>(); CK(cudaDeviceSynchronize());
    const int L = 20000;
    t0 = now();
    for (int i = 0; i < L; i++) empty_kernel<<<1, 1>>>();
    CK(cudaDeviceSynchronize());
    t1 = now();
    add(j, "async_launches_per_s", L / (t1 - t0), 0);
    const int R2 = 500;
    t0 = now();
    for (int i = 0; i < R2; i++) { empty_kernel<<<1, 1>>>(); CK(cudaDeviceSynchronize()); }
    t1 = now();
    add(j, "launch_sync_us", (t1 - t0) / R2 * 1e6, 1);
    int small = 0;
    t0 = now();
    for (int i = 0; i < R2; i++) CK(cudaMemcpy(&small, dv, 4, cudaMemcpyDeviceToHost));
    t1 = now();
    add(j, "dtoh_4b_us", (t1 - t0) / R2 * 1e6, 1);

    for (int s = 0; s < NS; s++) { cudaStreamDestroy(st[s]); cudaEventDestroy(ev[s]); }
    cudaFree(dx); cudaFree(dy); cudaFree(dv); cudaFree(dA); cudaFree(dB); cudaFree(dC); cudaFree(dd);
    cudaFree(dz); cudaFree(db); cudaFreeHost(hp); cudaFreeHost(hb);
    addb(j, "ok", R.ok);
    j += "}";
    return R;
}

// one saxpy over 256 Mi floats, split evenly over all devices at the same time
static bool split_all(int ndev, std::string& j) {
    const size_t n = 256u << 20, per = n / ndev;
    std::vector<float> x(n), y(n);
    for (size_t i = 0; i < n; i++) { x[i] = float(i % 977); y[i] = float(i % 13); }
    std::vector<float*> dx(ndev), dy(ndev);
    std::vector<cudaStream_t> st(ndev);
    float scale = 1.f;
    double t0 = now();
    for (int d = 0; d < ndev; d++) {
        CK(cudaSetDevice(d));
        CK(cudaStreamCreate(&st[d]));
        CK(cudaMalloc(&dx[d], per * 4)); CK(cudaMalloc(&dy[d], per * 4));
        CK(cudaMemcpyToSymbol(g_scale, &scale, 4));
        CK(cudaMemcpyAsync(dx[d], x.data() + d * per, per * 4, cudaMemcpyHostToDevice, st[d]));
        CK(cudaMemcpyAsync(dy[d], y.data() + d * per, per * 4, cudaMemcpyHostToDevice, st[d]));
        saxpy<<<256, 256, 0, st[d]>>>(2.f, dx[d], dy[d], per);
        CK(cudaMemcpyAsync(y.data() + d * per, dy[d], per * 4, cudaMemcpyDeviceToHost, st[d]));
    }
    for (int d = 0; d < ndev; d++) { CK(cudaSetDevice(d)); CK(cudaStreamSynchronize(st[d])); }
    double t1 = now();
    size_t bad = 0;
    for (size_t i = 0; i < per * ndev; i++) if (y[i] != 2.f * float(i % 977) + float(i % 13)) bad++;
    char b[160];
    snprintf(b, sizeof b, "\"split\":{\"devices\":%d,\"elements\":%zu,\"seconds\":%.3f,\"ok\":%s}", ndev, per * ndev,
             t1 - t0, bad ? "false" : "true");
    j += b;
    for (int d = 0; d < ndev; d++) { cudaSetDevice(d); cudaFree(dx[d]); cudaFree(dy[d]); cudaStreamDestroy(st[d]); }
    return bad == 0;
}

int main(int argc, char** argv) {
    bool all = false; int dev = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) all = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = atoi(argv[++i]);
    }
    int ndev = 0; CK(cudaGetDeviceCount(&ndev));
    bool ok = true;
    std::string j = "{\"gpus\":" + std::to_string(ndev) + ",";
    if (all) {
        ok &= split_all(ndev, j);
        j += ",\"devices\":[";
        for (int d = 0; d < ndev; d++) { Result r = check_device(d); ok &= r.ok; j += (d ? "," : "") + r.json; }
        j += "]";
    } else {
        Result r = check_device(dev); ok &= r.ok;
        j += "\"devices\":[" + r.json + "]";
    }
    j += ",\"ok\":" + std::string(ok ? "true" : "false") + "}";
    printf("%s\n", j.c_str());
    return ok ? 0 : 1;
}
