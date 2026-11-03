/* Bare minimum fake NVML for Jetson (Tegra doesn't ship libnvidia-ml). Just
 * enough for nvbandwidth and NCCL. Devices come from the CUDA driver, NVLink
 * and NVML P2P just say "not supported". Only meant for the benchmarks. */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dlfcn.h>
typedef void *nvmlDevice_t;
typedef int (*i_fn)(unsigned);
static int (*cuInit_)(unsigned), (*cuCount_)(int *), (*cuBus_)(char *, int, int), (*cuAttr_)(int *, int, int);
static int cu(void) {
    if (cuInit_) return 1;
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL); if (!h) return 0;
    cuInit_ = dlsym(h, "cuInit"); cuCount_ = dlsym(h, "cuDeviceGetCount");
    cuBus_ = dlsym(h, "cuDeviceGetPCIBusId"); cuAttr_ = dlsym(h, "cuDeviceGetAttribute");
    return cuInit_ && cuInit_(0) == 0;
}
#define H(i) ((nvmlDevice_t)(long)((i) + 1))
#define I(d) ((int)(long)(d) - 1)
int nvmlInit_v2(void) { return cu() ? 0 : 999; }
int nvmlInit(void) { return nvmlInit_v2(); }
int nvmlShutdown(void) { return 0; }
int nvmlSystemGetDriverVersion(char *v, unsigned n) {
    FILE *f = fopen("/etc/nv_tegra_release", "r"); char b[128] = "Tegra";
    if (f) { int maj = 0; char rev[32] = ""; if (fscanf(f, "# R%d (release), REVISION: %31[0-9.]", &maj, rev) == 2) snprintf(b, sizeof b, "L4T R%d.%s", maj, rev); fclose(f); }
    strncpy(v, b, n); if (n) v[n - 1] = 0; return 0; }
int nvmlDeviceGetCount_v2(unsigned *n) { int c = 0; if (!cu() || cuCount_(&c)) return 999; *n = c; return 0; }
int nvmlDeviceGetCount(unsigned *n) { return nvmlDeviceGetCount_v2(n); }
int nvmlDeviceGetHandleByIndex_v2(unsigned i, nvmlDevice_t *d) { *d = H(i); return 0; }
int nvmlDeviceGetHandleByIndex(unsigned i, nvmlDevice_t *d) { *d = H(i); return 0; }
int nvmlDeviceGetIndex(nvmlDevice_t d, unsigned *i) { *i = I(d); return 0; }
int nvmlDeviceGetHandleByPciBusId_v2(const char *bus, nvmlDevice_t *d) {
    unsigned n = 0; char b[64];
    if (nvmlDeviceGetCount_v2(&n)) return 999;
    for (unsigned i = 0; i < n; i++)
        if (cuBus_(b, sizeof b, i) == 0 && strncasecmp(b, bus, 12) == 0) { *d = H(i); return 0; }
    return 6; /* NVML_ERROR_NOT_FOUND */ }
int nvmlDeviceGetHandleByPciBusId(const char *bus, nvmlDevice_t *d) { return nvmlDeviceGetHandleByPciBusId_v2(bus, d); }
int nvmlDeviceGetHandleByUUID(const char *u, nvmlDevice_t *d) { (void)u; *d = H(0); return 0; }
int nvmlDeviceGetCudaComputeCapability(nvmlDevice_t d, int *ma, int *mi) {
    if (!cu() || cuAttr_(ma, 75, I(d)) || cuAttr_(mi, 76, I(d))) return 999;
    return 0; }
int nvmlDeviceGetP2PStatus(nvmlDevice_t a, nvmlDevice_t b, int idx, int *st) { (void)a; (void)b; (void)idx; *st = 5; /* NOT_SUPPORTED */ return 0; }
int nvmlDeviceGetNvLinkState(nvmlDevice_t d, unsigned l, int *s) { (void)d; (void)l; (void)s; return 3; }
int nvmlDeviceGetNvLinkRemotePciInfo(nvmlDevice_t d, unsigned l, void *p) { (void)d; (void)l; (void)p; return 3; }
int nvmlDeviceGetNvLinkRemotePciInfo_v2(nvmlDevice_t d, unsigned l, void *p) { (void)d; (void)l; (void)p; return 3; }
int nvmlDeviceGetNvLinkCapability(nvmlDevice_t d, unsigned l, int c, unsigned *r) { (void)d; (void)l; (void)c; (void)r; return 3; }
int nvmlDeviceGetFieldValues(nvmlDevice_t d, int n, void *v) { (void)d; (void)n; (void)v; return 3; }
int nvmlDeviceSetCpuAffinity(nvmlDevice_t d) { (void)d; return 3; /* NVML_ERROR_NOT_SUPPORTED */ }
const char *nvmlErrorString(int r) { return r == 0 ? "success" : r == 3 ? "not supported (Jetson NVML stub)" : r == 6 ? "not found" : "error (Jetson NVML stub)"; }
