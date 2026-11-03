# New slave bring-up and NVIDIA benchmarks (2026-11-02, evening)

The decommissioned xavier32 (.171) is replaced by a new Jetson AGX Xavier at
**192.168.4.173** (also named `xavier32`). The master is still xavier (.166).

## 1. Bring-up of the new board

| step | result |
|---|---|
| L4T | R35.6.4, kernel 5.10.216-tegra (the master is on R35.6.5) |
| JetPack (SDK Manager) | **JetPack 5.1.6**, which matches R35.6.4, so no L4T packages were upgraded. SDK Manager 26.1.0.14392 in Docker, the same recipe as xavier32 (response file `~/Downloads/nvidia/xavier173_jp516.ini`). **18 components succeeded, 0 failed.** The container exited and removed itself. |
| installed | CUDA 11.4.19, cuDNN 8.6.0, TensorRT 8.5.2, VPI 2.4.8, OpenCV 4.5.4, DeepStream 6.3 |
| jtop | jetson-stats 7.2.2; service active and enabled; same config as the master (`jetson_clocks` at boot, fan profile "cool") |
| **RAM** | **genuine 32 GB.** Board id P2888-0004 (the 32 GB AGX Xavier module); 32,639,676 kB of physical memory; 27 GiB written and verified twice with **0 errors**, at 5.1 GB/s write and 4.5 GB/s read. See `devices/xavier32-173-slave/ram_check.txt`. |
| USB-C roles | The cable first came up with .173 as USB host. The Type-C controller (`ccgx-ucsi`) exposes `data_role` read-only, so no software role swap is possible. After replugging and flipping the connector at the .173 end, .166 came up as host. **If roles come up reversed, replug and flip the cable at the slave end.** |
| C2C | `setup_slave.sh --key <master key>`; the link came up on the first handshake. c2cd, c2c-gpud and c2c-gadget are active and enabled. |

## 2. T01–T11 on the new slave

The first run was 11/11 PASS (run `20261102-201508`), but the new board was on
default clocks. Its jtop did not yet have `jetson_clocks` at boot, which the
master and the old slave have. Some results were lower than with xavier32:

| | xavier32 (.171) | .173 default clocks | .173 with jetson_clocks |
|---|---|---|---|
| link RTT | 0.318 ms | 0.538 ms | **0.309 ms** |
| TCP each way | 3.710 / 3.718 Gbit/s | 3.712 / 3.714 | **3.711 / 3.718** |
| remote launches/s | 256,710 | 127,338 | **267,835** |
| remote sync | 352 µs | 391.6 µs | **354.3 µs** |
| pooled SGEMM / HGEMM | 2.786 / 20.53 TFLOPS | 2.03 / 15.07 | **2.778 / 20.51** |

**Final run `20261102-202845`: 11/11 PASS**

| test | result |
|---|---|
| T01 link | RTT 0.309 ms, TCP 3.711 / 3.718 Gbit/s |
| T02 inventory | 16 CPUs, 44.8 GiB RAM, 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores |
| T03 CPU pool | `xz` 44.96 s → 26.93 s on 16 cores (1.67×) |
| T04 remote paging | 8 GiB in slave RAM, write 0.167 GiB/s, page-in 0.138 GiB/s, 0 errors |
| T05 beyond physical | 20.5 GiB on the 14.5 GiB master, write 0.302 GiB/s, page-in 0.145 GiB/s, 0 errors |
| T06 GPU pool | 29.5 GiB GPU memory, FP32 2.787, SGEMM 2.778, HGEMM 20.514 TFLOPS, 232.7 GB/s |
| T07 RAM link cut | 1.5 s cut mid-read, data identical |
| T08 remote GPU | SGEMM 1.376/1.385, HGEMM 9.864/10.014 TFLOPS (remote/native); copies 469.1/459.6 MB/s; 267,835 launches/s; sync 354.3 µs |
| T09 two GPUs | split SAXPY verified; simpleMultiGPU, MonteCarloMultiGPU, deviceQuery pass |
| T10 samples | remote 48 pass / 1 waived / 0 fail; local 49/49 |
| T11 GPU link cut | job survived a 1.5 s cut, results verified |

## 3. NVIDIA's own interconnect benchmarks vs our numbers

These are NVIDIA's standard tools for NVLink/NVSwitch/PCIe, run unmodified
through `c2c-gpu run` (device 1 = slave GPU). Details and adaptations are in
`bench/nvidia/README.md`; raw output is in `bench/nvidia/results/`.

### 3.1 Same quantity, three measurements

| quantity | our T08 | bandwidthTest | nvbandwidth (CE) | p2pBandwidthLatencyTest |
|---|---|---|---|---|
| master host → remote GPU | 469.1 MB/s | 0.5 GB/s (1-decimal output) | **0.47 GB/s** | – |
| remote GPU → master host | 459.6 MB/s | 0.5 GB/s | **0.46 GB/s** | – |
| both directions at once | not measured | – | **0.47 + 0.46 GB/s** (full duplex, about 0.93 GB/s total) | – |
| GPU 0 ↔ GPU 1 (across boards) | – | – | waived (needs peer access) | **0.42–0.43 GB/s** each way; bidirectional 0.43 total |
| GPU 0 ↔ GPU 1 latency | sync 354 µs | – | – | **638–778 µs** (GPU), 708–852 µs (CPU-issued) |
| copy inside the remote GPU | membw 111 GB/s | **109.2 GB/s** (D2D) | – | **98.7–99.5 GB/s** |

### 3.2 What the comparison shows

1. **Our bandwidth numbers hold up.** NVIDIA's tools measure the same
   0.46–0.47 GB/s host↔remote-GPU rate as our T08 (465 MB/s). That is the
   link's ceiling of 3.72 Gbit/s.
2. **The link is full duplex for GPU traffic**, which our own tests never
   showed. nvbandwidth's bidirectional test moved 0.47 GB/s up and 0.46 GB/s
   down at the same time, about 0.93 GB/s in total.
3. **GPU↔GPU copies across boards waste the duplex.**
   p2pBandwidthLatencyTest gets 0.43 GB/s in *both* the unidirectional and
   bidirectional tests. Without peer access, CUDA stages a cross-device copy
   through master host memory: first remote → master, then master → GPU 0.
   The two directions do not overlap. **This is the first throughput target:**
   pipelining the staging could reach about 0.9 GB/s bidirectional.
4. **Cross-GPU latency is two link round trips** (638–778 µs), against our
   one-round-trip sync (354 µs). The TCP bypass would cut both.
5. **Work inside the remote GPU runs at native speed.** D2D copies reach
   109 GB/s, against 114.9 GB/s natively.

### 3.3 Native baselines (master GPU only, for scale)

| | master GPU (shared LPDDR4x) |
|---|---|
| bandwidthTest host↔GPU | 38.9 / 38.9 GB/s |
| bandwidthTest D2D | 114.9 GB/s |
| nvbandwidth host↔GPU (CE) | 38.97 / 38.97 GB/s; SM copy 53.8 GB/s |
| p2pBandwidthLatencyTest | 108.4 GB/s, latency 3.7 µs |

On a Jetson, "host↔GPU" means moving data within the same RAM, so 39 GB/s is
the speed of local memory, not of an interconnect. Over C2C, the remote GPU
gets about 1/80 of that.

### 3.4 What does not run, and why

| test | result | reason |
|---|---|---|
| nvbandwidth SM-copy tests (16–29), latency tests (30–31) | not run | Their kernels read and write host memory directly (zero-copy). A GPU on another board cannot address the master's RAM. |
| nvbandwidth D2D CE tests (4–7, 12–15) | waived by nvbandwidth | They need peer access between the GPUs. |
| unpatched nvbandwidth | `CUDA_ERROR_ILLEGAL_ADDRESS` | Its start latch is a host-memory flag polled by a GPU kernel (fixed with the 5-line benchmark patch). |
| NCCL all_reduce, default transports | `cudaHostRegister`: operation not supported | The SHM transport maps host memory into the GPU. |
| NCCL with P2P/SHM off (socket transport) | `CUDA_ERROR_ILLEGAL_ADDRESS` | NCCL kernels talk to the proxy through host-mapped FIFOs. |
| NCCL on one local GPU | runs | sanity check: the NCCL build is good |

**Conclusion.** Collective libraries like NCCL need a GPU that can load and
store into host or peer memory. Our link gives copy semantics only. Making
NCCL work would need either:
- a custom NCCL net plugin, where the proxy on each board copies between its
  own GPU and the link and the GPU only touches local memory; or
- remote-mapped memory, which this hardware does not have.

The net plugin is a feasible later project (NCCL's `ncclNet` API is public).
It would also give NCCL multi-board collectives over the link.

## 4. State after testing

- Both nodes: c2cd active, link up. Slave: c2c-gpud active, ELPG restored to 1.
- No swap left attached; the kernel settings changed by c2c-rmem were restored.
- The benchmark tools are in `~/c2c-bench` on the master, outside the project.
  The user-local CMake is in `~/.local`; `libboost-program-options-dev` and
  `python3-pip` were installed with apt on the master.
- The old slave's test host key on the master was moved to
  `/etc/c2c/test_known_hosts.xavier32-old` (not deleted).
