// c2c-gpu-ls: prints the GPUs a CUDA program would see, one JSON object each.
// Run it via `c2c-gpu run` if you want the peer's GPU in the list too.
#include <cuda.h>
#include <cstdio>

int main() {
    if (cuInit(0) != CUDA_SUCCESS) { std::puts("[]"); return 1; }
    int n = 0;
    cuDeviceGetCount(&n);
    std::printf("[");
    for (int i = 0; i < n; i++) {
        CUdevice d;
        cuDeviceGet(&d, i);
        char name[256] = {0}, bus[32] = {0};
        cuDeviceGetName(name, sizeof name, d);
        cuDeviceGetPCIBusId(bus, sizeof bus, d);
        size_t mem = 0;
        cuDeviceTotalMem(&mem, d);
        int a[6] = {0};
        CUdevice_attribute at[6] = {CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                                    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                                    CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                                    CU_DEVICE_ATTRIBUTE_CLOCK_RATE, CU_DEVICE_ATTRIBUTE_INTEGRATED,
                                    CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID};
        for (int k = 0; k < 6; k++) cuDeviceGetAttribute(&a[k], at[k], d);
        std::printf("%s\n {\"ordinal\":%d,\"name\":\"%s\",\"cc\":\"%d.%d\",\"sms\":%d,\"clock_mhz\":%d,"
                    "\"total_mem\":%zu,\"pci\":\"%s\",\"remote\":%s}",
                    i ? "," : "", i, name, a[0], a[1], a[2], a[3] / 1000, mem, bus,
                    a[5] == 0xc2c ? "true" : "false");
    }
    std::printf("\n]\n");
    return 0;
}
