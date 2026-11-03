// c2c-gpu-bench: benchmark for the remote GPU data path (run under c2c-gpu run)
//
//   c2c-gpu run c2c-gpu-bench [-d DEV] [-m MiB] [-n ITERS] [--json]
//
// DEV defaults to the last device, i.e. the remote GPU if the link is up.
// All with pinned host memory:
//   h2d / d2h        one direction at a time
//   bidir            both directions at once on two streams. link is full
//                    duplex so this should get close to 2x one direction
//   sync_us          round trip of an empty cuCtxSynchronize
//   read4_us         4 byte device-to-host copy
//   peer_*           device 0 <-> DEV copies (if DEV != 0)
// Driver API only, so no nvcc needed to build it.
#include <cuda.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#define CK(x)                                                                        \
    do {                                                                             \
        CUresult r_ = (x);                                                           \
        if (r_ != CUDA_SUCCESS) {                                                    \
            const char* e_ = "?";                                                    \
            cuGetErrorString(r_, &e_);                                               \
            std::fprintf(stderr, "c2c-gpu-bench: %s failed: %s\n", #x, e_);          \
            std::exit(1);                                                            \
        }                                                                            \
    } while (0)

struct Lat {
    double avg, p50, p99;
};
static Lat lat(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    return {s / v.size() * 1e6, v[v.size() / 2] * 1e6, v[v.size() * 99 / 100] * 1e6};
}

int main(int argc, char** argv) {
    int dev = -1, mib = 256, iters = 2000;
    bool json = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-d" && i + 1 < argc) dev = std::atoi(argv[++i]);
        else if (a == "-m" && i + 1 < argc) mib = std::atoi(argv[++i]);
        else if (a == "-n" && i + 1 < argc) iters = std::atoi(argv[++i]);
        else if (a == "--json") json = true;
        else {
            std::fprintf(stderr, "usage: c2c-gpu-bench [-d DEV] [-m MiB] [-n ITERS] [--json]\n");
            return 2;
        }
    }
    CK(cuInit(0));
    int n = 0;
    CK(cuDeviceGetCount(&n));
    if (n == 0) {
        std::fprintf(stderr, "c2c-gpu-bench: no GPU\n");
        return 1;
    }
    if (dev < 0) dev = n - 1;
    CUdevice d;
    CK(cuDeviceGet(&d, dev));
    char name[128];
    CK(cuDeviceGetName(name, sizeof name, d));
    int dom = 0;
    cuDeviceGetAttribute(&dom, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, d);
    CUcontext ctx;
    CK(cuDevicePrimaryCtxRetain(&ctx, d));
    CK(cuCtxSetCurrent(ctx));

    const size_t bytes = size_t(mib) << 20;
    const size_t chunk = std::min<size_t>(bytes, 64u << 20);
    void *up, *down;
    CUdeviceptr da, db;
    CK(cuMemHostAlloc(&up, bytes, 0));
    CK(cuMemHostAlloc(&down, bytes, 0));
    std::memset(up, 0x5a, bytes);
    CK(cuMemAlloc(&da, bytes));
    CK(cuMemAlloc(&db, bytes));
    CUstream s1, s2;
    CK(cuStreamCreate(&s1, CU_STREAM_NON_BLOCKING));
    CK(cuStreamCreate(&s2, CU_STREAM_NON_BLOCKING));

    auto h2d = [&](CUstream s) {
        for (size_t o = 0; o < bytes; o += chunk)
            CK(cuMemcpyHtoDAsync(da + o, static_cast<char*>(up) + o, std::min(chunk, bytes - o), s));
    };
    auto d2h = [&](CUstream s) {
        for (size_t o = 0; o < bytes; o += chunk)
            CK(cuMemcpyDtoHAsync(static_cast<char*>(down) + o, db + o, std::min(chunk, bytes - o), s));
    };
    // warm up
    CK(cuMemcpyHtoD(db, up, chunk));
    CK(cuCtxSynchronize());

    double t = now();
    h2d(s1);
    CK(cuStreamSynchronize(s1));
    double t_h2d = now() - t;
    t = now();
    d2h(s1);
    CK(cuStreamSynchronize(s1));
    double t_d2h = now() - t;
    t = now();
    d2h(s2);  // start downloads first so they stream back while the uploads go out
    h2d(s1);
    CK(cuStreamSynchronize(s1));
    CK(cuStreamSynchronize(s2));
    double t_bi = now() - t;

    std::vector<double> vs, vr;
    int one = 0;
    for (int i = 0; i < 100; i++) CK(cuCtxSynchronize());
    for (int i = 0; i < iters; i++) {
        double a = now();
        CK(cuCtxSynchronize());
        vs.push_back(now() - a);
    }
    for (int i = 0; i < iters; i++) {
        double a = now();
        CK(cuMemcpyDtoH(&one, db, 4));
        vr.push_back(now() - a);
    }
    Lat ls = lat(vs), lr = lat(vr);

    double peer_to = 0, peer_from = 0;
    if (dev != 0 && n > 1) {  // device 0 <-> DEV, the driver stages it
        CUdevice d0;
        CUcontext c0;
        CK(cuDeviceGet(&d0, 0));
        CK(cuDevicePrimaryCtxRetain(&c0, d0));
        CK(cuCtxSetCurrent(c0));
        CUdeviceptr l;
        CK(cuMemAlloc(&l, bytes));
        CK(cuMemsetD8(l, 1, bytes));
        CK(cuCtxSynchronize());
        t = now();
        CK(cuMemcpyPeer(da, ctx, l, c0, bytes));
        peer_to = bytes / (now() - t) / 1e6;
        t = now();
        CK(cuMemcpyPeer(l, c0, db, ctx, bytes));
        peer_from = bytes / (now() - t) / 1e6;
        cuMemFree(l);
        CK(cuCtxSetCurrent(ctx));
    }
    double mb = bytes / 1e6;
    if (json) {
        std::printf("{\"device\":%d,\"name\":\"%s\",\"pci_domain\":%d,\"mib\":%d,"
                    "\"h2d_mbs\":%.1f,\"d2h_mbs\":%.1f,\"bidir_mbs\":%.1f,"
                    "\"sync_us\":%.1f,\"sync_p50_us\":%.1f,\"sync_p99_us\":%.1f,"
                    "\"read4_us\":%.1f,\"read4_p50_us\":%.1f,\"read4_p99_us\":%.1f,"
                    "\"peer_to_mbs\":%.1f,\"peer_from_mbs\":%.1f}\n",
                    dev, name, dom, mib, mb / t_h2d, mb / t_d2h, 2 * mb / t_bi, ls.avg, ls.p50, ls.p99,
                    lr.avg, lr.p50, lr.p99, peer_to, peer_from);
    } else {
        std::printf("device %d: %s%s, %d MiB per test\n", dev, name, dom == 0xc2c ? " (remote, over C2C)" : "", mib);
        std::printf("  host -> GPU        %8.1f MB/s\n", mb / t_h2d);
        std::printf("  GPU -> host        %8.1f MB/s\n", mb / t_d2h);
        std::printf("  both at once       %8.1f MB/s total\n", 2 * mb / t_bi);
        std::printf("  sync round trip    avg %6.1f  p50 %6.1f  p99 %6.1f us\n", ls.avg, ls.p50, ls.p99);
        std::printf("  4-byte read        avg %6.1f  p50 %6.1f  p99 %6.1f us\n", lr.avg, lr.p50, lr.p99);
        if (peer_to > 0)
            std::printf("  GPU0 -> GPU%d       %8.1f MB/s\n  GPU%d -> GPU0       %8.1f MB/s\n", dev, peer_to, dev,
                        peer_from);
    }
    return 0;
}
