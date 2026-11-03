# Remote GPU (c2c-gpu)

The slave's GPU becomes an ordinary CUDA device on the master. Unmodified
CUDA programs started with `c2c-gpu run` see:

| ordinal | GPU | where |
|---|---|---|
| 0 | Xavier (8 SM, 16 GB) | the master itself |
| 1 | Xavier (8 SM, 32 GB) | the slave, over the C2C link (`pciDomainID` 0xc2c) |

```bash
c2c gpu ls                                   # list the GPUs a program sees
c2c gpu run ./my_cuda_app                    # both GPUs (device 1 = slave)
CUDA_VISIBLE_DEVICES=1 c2c gpu run ./my_app  # only the slave's GPU, as device 0
C2C_GPU_STATS=1 c2c gpu run ./my_app         # traffic summary at exit (shows the transport)
sudo c2c gpu system on                       # every CUDA program sees the slave's GPU
c2c gpu bench                                # bandwidth (each way, both at once) + latency
```

The program is not recompiled, relinked or modified. Programs built with the
CUDA runtime (static or shared), the driver API, cuBLAS, cuFFT, cuRAND and
cuSPARSE work.

## How it works

```
master                                               slave
 app ── CUDA runtime / cuBLAS ── libcuda.so.1 (libc2ccuda) ══ TCP over c2c0 ══ c2c-gpud ── real libcuda ── GPU
                                     │                                         (one process per app)
                                     └── NVIDIA libcuda ── local GPU (device 0, untouched)
```

* **`libc2ccuda`** replaces `libcuda.so.1` in programs started by
  `c2c-gpu run`. It loads NVIDIA's driver library and forwards everything
  for the local GPU unchanged. All 533 driver entry points are exported:
  127 are implemented for the remote GPU, and the rest go to a generated
  trampoline. If the calling thread's context belongs to the remote GPU, an
  unsupported call returns `CUDA_ERROR_NOT_SUPPORTED`; it never silently
  runs on the wrong GPU.
* **Shadow context.** Each remote context is backed by a real context on
  the local GPU (same GV11B GPU). The CUDA runtime's queries, module
  bookkeeping, occupancy calculator and NVIDIA's undocumented internal
  tables therefore get correct answers locally. Only the data plane crosses
  the link: allocations, copies, memsets, kernel launches, streams, events,
  arrays, textures and synchronisation.
* **Unique device pointers.** Both drivers place allocations at the same
  addresses (`0x203...`). Master and slave therefore agree on one window of
  GPU virtual address space that is free in both processes, using CUDA's
  virtual memory management API (`cuMemAddressReserve` / `cuMemMap`). Every
  remote allocation is mapped on the slave at exactly the address the master
  handed out. Pointers can be stored in structs and passed to kernels
  unchanged.
* **Kernel parameters.** Each kernel's parameter layout is read from its
  cubin (`EIATTR_KPARAM_INFO`, including NVIDIA's compressed fatbins) or its
  PTX, so `cuLaunchKernel(..., void** kernelParams, ...)` can be serialised.
  The parser was checked against `cuobjdump` on all 3,879 sm_72 kernels in
  cuBLAS: every one matched.
* **No round trip unless needed.** Creating streams, events, modules and
  functions, launches, uploads and memsets are asynchronous. They are
  batched into few TCP segments and flushed within 100 µs. Only calls that
  must return data or wait (synchronize, download, `cudaMalloc`, queries)
  wait for the slave. Errors from asynchronous work are reported by the
  next synchronising call, as CUDA does.
* **Modules are sent once.** Only the image for the slave's architecture
  (sm_72) is sent, and the slave caches it by content hash in
  `/var/cache/c2c-gpud`. cuBLAS's ~500 fatbinaries are loaded lazily, only
  when one of their kernels is first used remotely.
* **`c2c-gpud`** (systemd service on the slave) forks one process per
  application, so a crash or leak cannot affect other applications and
  everything is freed when the application exits. It listens only on the
  link address and only accepts the master. While a client is connected it
  turns off the GPU's engine power gating (ELPG), which otherwise adds about
  250 µs of wake-up time to every synchronous call, and restores it
  afterwards.

## Transport

| `gpu_transport` | path | sync round trip | survives a USB cut |
|---|---|---|---|
| `raw` | `/dev/c2cchN`: own USB interface → `c2c-gpud` (no TCP/IP, TUN, c2cd) | **~140 µs** | no: the program gets an error (T13) |
| `tcp` | TCP over `c2c0` (c2cd, TUN) | ~340 µs | yes, short cuts (T11) |
| `auto` (default) | a free raw channel, else TCP | | |

- **Channels** are exclusive: a program holds one while it runs. The slave
  exposes `raw_channels` of them (default 4); program number 5 uses TCP.
- **Busy-polling:** both sides spin for `gpu_spin_us` / `gpud_spin_us`
  (1 ms) before sleeping. That costs one core while a program actively talks
  to the remote GPU.
- **Full duplex:**
  - downloads issued with an `*Async` API into page-locked memory overlap
    with uploads and launches;
  - the server buffers up to `gpud_download_buffer_mib` (512) of downloads
    in flight.
- **System-wide mode:**
  - `c2c gpu system on` (`gpu_system_wide=yes`) adds
    `/etc/ld.so.conf.d/00-c2c-gpu.conf`, so every CUDA program loads this
    front;
  - a program that never touches device 1 costs the slave a forked process
    and no CUDA context (sessions start CUDA lazily);
  - with the link down, programs start at once with the local GPU only.

## Measured: raw channels (T12, 2026-11-02)

| | raw channel | TCP | old build (TCP) |
|---|---|---|---|
| upload / download | 471 / 460 MB/s | 459 / 462 | 459 / 462 |
| both at once | **872 MB/s** | 761 | 464 |
| sync p50 / p99 | **138 / 170 µs** | 343 / 400 | 326 / 406 |
| 4-byte read p50 | **160 µs** | 367 | 376 |
| GPU0 ↔ GPU1 | **469 / 466 MB/s** | | 422 / 434 |

## Measured (Xavier ↔ Xavier, 2026-11-02, before the raw channels)

| | native on slave | remote from master |
|---|---|---|
| FP32 FMA | 1.396 TFLOPS | 1.396 TFLOPS |
| cuBLAS SGEMM 4096³ | 1.392 TFLOPS | 1.383 TFLOPS |
| tensor-core HGEMM 4096³ | 10.2 TFLOPS | 10.14 TFLOPS |
| device memory bandwidth | 114 GB/s | 114.9 GB/s |
| largest allocation | - | 20 GiB |
| host → GPU copy | - | 470 MB/s (the link's limit) |
| GPU → host copy | - | 460 MB/s |
| async kernel launches | 208k/s | 243k/s |
| launch + synchronize | 10.7 µs | 356 µs (link TCP round trip: 369 µs) |

Compute on the remote GPU runs at native speed. Data movement is bounded by
the link (3.7 Gbit/s, about 0.37 ms round trip): keep data on the GPU and
move it in large blocks.

## Not supported on the remote GPU

These calls return `CUDA_ERROR_NOT_SUPPORTED`, or the matching device
attribute reads 0 so programs choose another path:

* unified/managed memory (`cudaMallocManaged`), zero-copy mapped host memory
  (`cudaHostGetDevicePointer`), peer access between the two GPUs (copies
  between them work and are staged through host memory);
* CUDA graphs, stream capture, memory pools / `cudaMallocAsync`,
  IPC handles, multi-device cooperative launch, mipmapped arrays,
  texture/surface *references* (texture/surface *objects* work);
* device-side `printf` output appears in the slave's `c2c-gpud` log, not in
  the application's terminal.

Several threads may use the remote GPU concurrently, but their calls are
serialised over one connection.

A raw-channel session ends with the program if the USB link drops (the
remote GPU state lives in the session). Use `gpu_transport=tcp` for jobs that
must ride out cable cuts.

## Files

| file | role |
|---|---|
| `libc2ccuda.cpp` | the libcuda front (master) |
| `gen_tramp.py` | generates the trampolines for all driver entry points from NVIDIA's `libcuda` |
| `fatbin.cpp` | fatbinary / cubin / PTX parsing, LZ4-style decompression |
| `c2c-gpud.cpp` | the server (slave) |
| `gpu_proto.hpp` | wire protocol |
| `c2c-gpu` | launcher script |
| `c2c-gpu-ls.cpp` | lists the GPUs a program sees |
| `c2c-gpu-bench.cpp` | bandwidth / duplex / latency benchmark (driver API, no nvcc) |
| `fatdump.cpp` | validation tool for the parameter parser (compare with `cuobjdump -elf`) |
