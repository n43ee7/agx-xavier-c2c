# AGX Xavier C2C
AI Inference is the new revolution. Following the concept of NVIDIA DGX Spark where multiple Spark machines can be connected together to create a unified compute machine, this is an attempt to make a similar program that can make it happen on 2 NVIDIA Jetson AGX Xavier machines. A very traditional setup can be using the Ethernet port of both the devices in a Cluster setup but my goal was a little different but maybe I will later add that feature into the C2C Protocol.

## Types of Possible C2C Channels 
1. USB-C (C2C-C)
2. PCIe 8x (C2C-PCIE)

## C2C Architecture
This protocol is a 1 Root 1 Client model where there is one Master that connects to the Slave machine and establish a handshake to commence transfers. The Master node generally invokes transactions to the Slave node. The C2C protocol is packaged as a linux system service that constantly runs until the connection is aborted due to any issue. The protocl is installed as a kernel object to the master node and the slave node (each with a fixed purpose). 

NOTE TO MYSELF: I will try to draw and stuff here to explain a bit better

## C2C Capabilities
The table below shows the performance of different C2C Modes to get an idea of what are the limitations

| | C2C-C (USB-C) | C2C-PCIE |
|---|---|---|
| Link speed | USB 3.1 Gen 1, 5 Gbit/s (the Xavier's USB device side can't do more) | |
| Bandwidth per direction | ~470 MB/s | |
| Both directions at once | ~870 MB/s | |
| Round trip (remote GPU sync) | ~140 µs | |
| Remote GPU | shows up as CUDA device 1 | |
| Remote RAM | shows as swap (~0.15 GB/s read back) | |
| Remote CPU | per job (`c2c cpu run / pipe`) | |
| Survives a cable cut | TCP mode yes, raw mode no (program gets an error) | |
| Hardware needed | one USB-C cable | |

### USB-C (C2C-C):
This C2C methodology removes all kinds layers guarding the USB Protocol and simply uses raw USB-C to expose 4 raw data channels which we communicate over. The philosophy behind this methodology was for cases where there is not a lot of simultaneous GPU read and writes (i.e. having an LLM model stuffed in the memory) and running it makes sense. Also if the PCIe ports are being utilized for external compute appliances, the USB-C method seems to be the only option...

NOTE TO MYSELF: Include them drawings here

### PCIe (C2C-PCIE):
Probably the most preferred method here since according to NVIDIA documentations the PCIe Root complex on the AGX Xavier is a configurable endpoint and it can have speeds up to ~15.3 GiB/s so the assumption is that this will eradicate all channel based limitations and hit us with the irreplaceable limitation of the CPU. This medium though requires a bit of a hardware change. You will have to buy a PCIe Male to Male cable WITH RX/TX Swapped for it to work properly else you might fry the board.

NOTE TO MYSELF: Include them drawings here

## C2C Benchmarks
The benchmarks of these tests were carried out by putting both the boards on MAXN Performance mode and enabling the Jetson Clocks so respectively if you are running a slower performance model your statistics might differ. Also a good majority of these also depend on the quality of the medium (a.k.a the USB-C cable and the PCIe cable). I used a USB-C Thunderbolt cable to do the C2C-C tests and the PCIe tests with a 15 cm rx/tx swapped cable that I found on Amazon. You can probably get something better and try it out and let me know. NOTE: Jetson AGX Xavier does not have a thunderbolt port btw. It just has a USB-C 3.1 port on it.
### C2C Over USB-C (C2C-C)
All of these come from the proof test suite in `c2c-c/tests` (13 tests, last full run 13/13 PASS) plus `c2c gpu bench`. Master is the 16 GB Xavier, Slave is the 32 GB one. Each number is a single run unless I say otherwise, so expect a few % of wobble between runs.

**The link itself**
| Test | Result |
|---|---|
| TCP over the link (`c2c0`), each direction | 3.71 / 3.72 Gbit/s |
| TCP both directions at once (60 s)* | 3.45 + 3.44 = 6.89 Gbit/s, 0 retransmits |
| Ping round trip | 0.31 ms |
| Raw USB ceiling (in-kernel benchmark, no daemon)* | 449.5 MiB/s per direction (~94% of the line rate) |

\* measured on Nov 1 with my first Slave board (also a 32 GB AGX Xavier) before I swapped it for the current one. Everything else is the current pair.

**Remote GPU (Slave's GPU used from the Master, raw channels)**
| Test | Remote | Native on the Slave |
|---|---|---|
| cuBLAS SGEMM | 1.382 TFLOPS | 1.383 TFLOPS |
| Tensor core HGEMM | 10.09 TFLOPS | 10.04 TFLOPS |
| Upload / download | 471 / 460 MB/s | |
| Upload + download at the same time | 872 MB/s total | |
| GPU0 ↔ GPU1 (across the boards) | 469 / 466 MB/s | |
| Sync round trip (p50 / p99) | 138 / 170 µs | ~8 µs |
| 4 byte read back (p50) | 160 µs | |
| Kernel launches | 255k per second | |
| NVIDIA CUDA samples | 48 of 49 pass (the 49th needs unified memory) | 49 of 49 |

**Everything pooled**
| Test | Result |
|---|---|
| Inventory seen by the Master | 16 CPUs, 44.8 GiB RAM, 2 GPUs (16 SMs, 1024 CUDA cores, 128 tensor cores) |
| Both GPUs working together | 2.79 TFLOPS FP32, 20.5 TFLOPS FP16 tensor, 231 GB/s memory |
| CPU pool (`xz` over 512 MiB, 64 chunks) | 43.2 s on 8 cores → 25.8 s on 16 cores (1.68x) |
| One process bigger than the Master's RAM | 20.5 GiB used on the 14.5 GiB Master, 12 GiB of it sitting in the Slave's RAM, 0 errors |
| Remote RAM speed through swap | ~0.26-0.35 GiB/s write, ~0.14-0.15 GiB/s read back |

### C2C Over PCIe (C2C-PCIE)

## Comparison
I wanted to keep this honest so everything here was measured on the same two boards on the same day unless it says otherwise. Nothing in these tables is a datasheet number except the two line rates.

**C2C-C vs the other ways of connecting these two boards**
| Connection | Per direction | Round trip | How I measured it |
|---|---|---|---|
| C2C-C raw channel | 470 MB/s | 138 µs (GPU sync), 145-160 µs (raw ping) | `c2c gpu bench`, `rawpp` |
| C2C-C TCP over `c2c0` | 465 MB/s (3.72 Gbit/s) | 310-340 µs | `iperf3`, ping, `c2c gpu bench` in TCP mode |
| Stock L4T USB networking (RNDIS/NCM), same cable | 65-75 MB/s (0.52 / 0.60 Gbit/s) | | `iperf3`, Nov 1, first Slave board |
| Gigabit Ethernet through my LAN switch | 118 MB/s | 698 µs | simple TCP test between the boards |
| Line rate, USB 3.1 Gen 1 (theory) | 500 MB/s raw (5 Gbit/s, 8b/10b) | | spec |
| Line rate, 1 GbE (theory) | 125 MB/s | | spec |

So C2C-C is about 4x the bandwidth of the Ethernet port (and the raw channels have roughly 4-5x lower latency than it), and about 7x faster than the USB networking NVIDIA ships on the same cable. To be fair to Ethernet, my test went through a LAN switch, a direct cable would shave some latency off.

**C2C-C before vs after the TCP bypass (same boards, measured back to back)**
| Test | Old (everything over TCP) | Now (raw channels) |
|---|---|---|
| Remote GPU sync | 326 µs | 143 µs |
| 4 byte read back | 376 µs | 167 µs |
| Upload + download at once | 464 MB/s | 872 MB/s |
| GPU0 ↔ GPU1 | 422 / 434 MB/s | 466 / 466 MB/s |

**NVIDIA's own benchmarks run over C2C-C**
| Tool | Over C2C-C | Same tool, local GPU only |
|---|---|---|
| `bandwidthTest` (pinned) | 0.5 GB/s each way (it rounds to 0.1) | 38.9 GB/s |
| `nvbandwidth` copy engine | 0.47 / 0.47 GB/s, both at once 0.47 + 0.50 GB/s | 38.97 GB/s |
| `p2pBandwidthLatencyTest` GPU0 ↔ GPU1 | 0.47 GB/s, 527 µs GPU1→GPU0 latency | 108 GB/s, 3.7 µs (same GPU) |
| NCCL (`all_reduce_perf`) | does not run across the boards | runs on one GPU |

Things to keep in mind when reading these:
- The Xavier's own memory is roughly 80x faster than the cable. A remote GPU computes at full native speed once the data is over there, but anything that keeps shuffling data back and forth is going to feel the cable.
- `nvbandwidth` needed a 5 line patch (it polls a flag in host memory that a GPU on another board just can't see) and an NVML stub since Jetson doesn't ship NVML. Both are in `c2c-c/bench/nvidia`.
- NCCL wants the GPU to read and write the other side's memory directly. That's not something a cable like this can do so it fails cleanly instead.
- Latency on USB moves in ~125 µs steps (the USB bus interval) so round trips land on 160 / 250 / 380 µs depending on timing. That's the hardware, not the code.

## The catch
I don't want to write a new Linux kernel from scratch, so applications currently need to invoke `c2c cpu`, `c2c gpu`, and similar commands to use the shared compute resources. Two of the three pieces are now mostly solved: `sudo c2c gpu system on` makes every CUDA program on the Master see the Slave's GPU without a wrapper, and using `rmem_lend_gib` on the Slave together with `rmem_auto=yes` on the Master attaches the Slave's RAM as swap at boot, giving the OS more memory. The CPUs are still the odd one out; making the scheduler see all 16 cores requires patching the kernel, so CPU sharing remains per job through `c2c cpu run` and `c2c cpu pipe`. I am open to ideas for making all shared resources persistently available without requiring applications to invoke them differently.

### Footnote
This program is not supplied with any warranties nor is it an endorsement of any products. It just happens to be the case where I got these 2 Xaviers laying around and I wanted to run an LLM on it so why not do this while I am at it too. 
