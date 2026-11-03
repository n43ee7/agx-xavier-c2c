// latbench: latency spread for launch+sync and a 4 byte DtoH (run it under c2c-gpu)
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cuda_runtime.h>
__global__ void k() {}
static double us() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void report(const char* n, std::vector<double>& v) {
    std::sort(v.begin(), v.end()); double s = 0; for (double x : v) s += x;
    printf("%-12s avg %7.1f  p50 %7.1f  p90 %7.1f  p99 %7.1f  max %8.1f us\n", n, s / v.size(), v[v.size() / 2], v[v.size() * 9 / 10], v[v.size() * 99 / 100], v.back());
}
int main(int argc, char** argv) {
    int dev = argc > 1 ? atoi(argv[1]) : 0, N = argc > 2 ? atoi(argv[2]) : 2000;
    cudaSetDevice(dev);
    int* d; int h = 0; cudaMalloc(&d, 4);
    for (int i = 0; i < 200; i++) { k<<<1, 1>>>(); cudaDeviceSynchronize(); }
    std::vector<double> a, b, c, e;
    for (int i = 0; i < N; i++) { double t = us(); cudaDeviceSynchronize(); c.push_back(us() - t); }
    for (int i = 0; i < N; i++) { double t = us(); k<<<1, 1>>>(); cudaDeviceSynchronize(); a.push_back(us() - t); }
    for (int i = 0; i < N; i++) { double t = us(); cudaMemcpy(&h, d, 4, cudaMemcpyDeviceToHost); b.push_back(us() - t); }
    for (int i = 0; i < N; i++) { double t = us(); k<<<1, 1>>>(); cudaMemcpy(&h, d, 4, cudaMemcpyDeviceToHost); e.push_back(us() - t); }
    report("sync only", c); report("launch+sync", a); report("dtoh 4B", b); report("launch+dtoh", e);
    return 0;
}
