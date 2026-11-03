// gpu_probe - plain CUDA program that inventories one GPU and pushes it a bit.
// No C2C in here, the tests just run a copy on each board.
//
//   gpu_probe --info            device properties (JSON)
//   gpu_probe --bench [-m F]    properties + measured capability (JSON):
//       max_alloc   biggest allocation one process can hold (and touch)
//                   (F = fraction of free memory to try, default 0.85)
//       fp32_fma    FP32 FMA rate on the CUDA cores
//       sgemm       cuBLAS SGEMM 4096^3
//       hgemm_tc    cuBLAS FP16 GEMM 4096^3 on the tensor cores
//       membw       D2D copy bandwidth
#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
    fprintf(stderr, "CUDA %s: %s\n", #x, cudaGetErrorString(e_)); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t s_ = (x); if (s_ != CUBLAS_STATUS_SUCCESS) { \
    fprintf(stderr, "cuBLAS %s: %d\n", #x, (int)s_); exit(1); } } while (0)

static int cores_per_sm(int major, int minor) {
    switch (major * 10 + minor) {
    case 53: case 62: return 128; case 60: return 64; case 61: return 128;
    case 70: case 72: case 75: return 64; case 80: return 64; case 86: case 87: return 128;
    default: return 64;
    }
}
static int tc_per_sm(int major) { return major >= 7 ? 8 : 0; }

__global__ void fma_kernel(float* out, int iters) {
    float a = threadIdx.x * 1e-3f, b = 1.0001f, c = 0.9999f, d = blockIdx.x * 1e-4f;
    for (int i = 0; i < iters; ++i) {
        a = fmaf(a, b, c); d = fmaf(d, c, b);
        b = fmaf(b, a, d); c = fmaf(c, d, a);
    }
    if (a + b + c + d == 12345.f) out[0] = a;   // so the compiler can't throw the math away
}

__global__ void touch_kernel(uint4* p, size_t n) {
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
        p[i] = make_uint4((unsigned)i, 1, 2, 3);
}

static double elapsed_ms(cudaEvent_t a, cudaEvent_t b) {
    float ms;
    CK(cudaEventElapsedTime(&ms, a, b));
    return ms;
}

int main(int argc, char** argv) {
    bool bench = argc > 1 && !strcmp(argv[1], "--bench");
    double frac = 0.85;
    for (int i = 1; i < argc - 1; ++i) if (!strcmp(argv[i], "-m")) frac = atof(argv[i + 1]);

    int dev = 0, ndev = 0;
    CK(cudaGetDeviceCount(&ndev));
    cudaDeviceProp p;
    CK(cudaGetDeviceProperties(&p, dev));
    CK(cudaSetDevice(dev));
    int clk_khz = 0, memclk_khz = 0, buswidth = 0;
    cudaDeviceGetAttribute(&clk_khz, cudaDevAttrClockRate, dev);
    cudaDeviceGetAttribute(&memclk_khz, cudaDevAttrMemoryClockRate, dev);
    cudaDeviceGetAttribute(&buswidth, cudaDevAttrGlobalMemoryBusWidth, dev);
    size_t free_b, total_b;
    CK(cudaMemGetInfo(&free_b, &total_b));
    char host[64] = {0};
    gethostname(host, sizeof host - 1);
    int cps = cores_per_sm(p.major, p.minor);
    double peak_fp32 = 2.0 * p.multiProcessorCount * cps * clk_khz * 1e3 / 1e12;

    printf("{\"host\":\"%s\",\"gpus\":%d,\"name\":\"%s\",\"cc\":\"%d.%d\",\"sms\":%d,"
           "\"cuda_cores\":%d,\"tensor_cores\":%d,\"clock_mhz\":%d,\"mem_clock_mhz\":%d,"
           "\"mem_bus_bits\":%d,\"integrated\":%d,\"total_mem\":%zu,\"free_mem\":%zu,"
           "\"l2_bytes\":%d,\"peak_fp32_tflops\":%.3f",
           host, ndev, p.name, p.major, p.minor, p.multiProcessorCount,
           p.multiProcessorCount * cps, p.multiProcessorCount * tc_per_sm(p.major), clk_khz / 1000,
           memclk_khz / 1000, buswidth, p.integrated, total_b, free_b, p.l2CacheSize, peak_fp32);
    if (!bench) { printf("}\n"); return 0; }

    cudaEvent_t e0, e1;
    CK(cudaEventCreate(&e0));
    CK(cudaEventCreate(&e1));

    // 1. biggest allocation we can hold and actually use
    size_t step = 256UL << 20, want = (size_t)(free_b * frac) / step * step, got = 0;
    void* big = nullptr;
    while (want >= step) {
        if (cudaMalloc(&big, want) == cudaSuccess) { got = want; break; }
        cudaGetLastError();
        want -= step;
    }
    double touch_ms = 0;
    if (got) {
        CK(cudaEventRecord(e0));
        touch_kernel<<<p.multiProcessorCount * 32, 256>>>((uint4*)big, got / sizeof(uint4));
        CK(cudaEventRecord(e1));
        CK(cudaEventSynchronize(e1));
        touch_ms = elapsed_ms(e0, e1);
        CK(cudaFree(big));
    }

    // 2. FP32 FMA
    float* out;
    CK(cudaMalloc(&out, 4));
    int blocks = p.multiProcessorCount * 32, threads = 256, iters = 8192;
    fma_kernel<<<blocks, threads>>>(out, 64);
    CK(cudaEventRecord(e0));
    fma_kernel<<<blocks, threads>>>(out, iters);
    CK(cudaEventRecord(e1));
    CK(cudaEventSynchronize(e1));
    double fma_tflops = 2.0 * 4 * (double)iters * blocks * threads / (elapsed_ms(e0, e1) * 1e-3) / 1e12;

    // 3. cuBLAS SGEMM + tensor-core HGEMM, N=4096
    const int N = 4096, reps = 5;
    cublasHandle_t h;
    CB(cublasCreate(&h));
    float *A, *B, *C;
    CK(cudaMalloc(&A, sizeof(float) * N * N));
    CK(cudaMalloc(&B, sizeof(float) * N * N));
    CK(cudaMalloc(&C, sizeof(float) * N * N));
    CK(cudaMemset(A, 0, sizeof(float) * N * N));
    CK(cudaMemset(B, 0, sizeof(float) * N * N));
    float one = 1.f, zero = 0.f;
    CB(cublasSgemm(h, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &one, A, N, B, N, &zero, C, N));
    CK(cudaEventRecord(e0));
    for (int r = 0; r < reps; ++r)
        CB(cublasSgemm(h, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &one, A, N, B, N, &zero, C, N));
    CK(cudaEventRecord(e1));
    CK(cudaEventSynchronize(e1));
    double sgemm_tflops = 2.0 * N * N * (double)N * reps / (elapsed_ms(e0, e1) * 1e-3) / 1e12;

    CB(cublasSetMathMode(h, CUBLAS_TENSOR_OP_MATH));
    __half *Ah = (__half*)A, *Bh = (__half*)B, *Ch = (__half*)C;
    __half hone = __float2half(1.f), hzero = __float2half(0.f);
    CB(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &hone, Ah, CUDA_R_16F, N, Bh, CUDA_R_16F, N,
                    &hzero, Ch, CUDA_R_16F, N, CUBLAS_COMPUTE_16F, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
    CK(cudaEventRecord(e0));
    for (int r = 0; r < reps * 2; ++r)
        CB(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &hone, Ah, CUDA_R_16F, N, Bh, CUDA_R_16F, N,
                        &hzero, Ch, CUDA_R_16F, N, CUBLAS_COMPUTE_16F, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
    CK(cudaEventRecord(e1));
    CK(cudaEventSynchronize(e1));
    double hgemm_tflops = 2.0 * N * N * (double)N * reps * 2 / (elapsed_ms(e0, e1) * 1e-3) / 1e12;

    // 4. memory bandwidth (counts read + write)
    size_t bw_bytes = sizeof(float) * N * N;
    CK(cudaMemcpy(B, A, bw_bytes, cudaMemcpyDeviceToDevice));
    CK(cudaEventRecord(e0));
    for (int r = 0; r < 20; ++r) CK(cudaMemcpy(B, A, bw_bytes, cudaMemcpyDeviceToDevice));
    CK(cudaEventRecord(e1));
    CK(cudaEventSynchronize(e1));
    double membw = 2.0 * bw_bytes * 20 / (elapsed_ms(e0, e1) * 1e-3) / 1e9;

    printf(",\"max_alloc\":%zu,\"max_alloc_touch_gbs\":%.2f,\"fp32_fma_tflops\":%.3f,"
           "\"sgemm_tflops\":%.3f,\"hgemm_tc_tflops\":%.3f,\"membw_gbs\":%.2f}\n",
           got, touch_ms > 0 ? got / (touch_ms * 1e-3) / 1e9 : 0.0, fma_tflops, sgemm_tflops,
           hgemm_tflops, membw);
    cublasDestroy(h);
    return 0;
}
