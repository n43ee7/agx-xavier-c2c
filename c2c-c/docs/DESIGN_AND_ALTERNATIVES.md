# usbc-c2c: why this design, and what was ruled out

This document covers the 2026-11-02 throughput work ("feature/usbc-raw-channels"). Every number was
measured on two Jetson AGX Xaviers (master xavier .166, 16 GB; slave xavier32 .173, 32 GB),
L4T R35.6.x, connected by one USB-C cable. Raw results are in `tests/results/` and
`bench/nvidia/results/`.

## 1. Where the time went (before)

| Path, 16-byte round trip | Measured | What it contains |
|---|---|---|
| In-kernel ping-pong (host driver ↔ gadget loopback) | 90–120 µs | USB only: the physical floor |
| User space ↔ user space over the raw device, blocking reads | 183–201 µs | + 4 thread wake-ups |
| User space ↔ user space, busy-polled | **144–160 µs** | + syscalls and copies |
| TCP request/response over c2c0 (c2cd + TUN + TCP, both sides) | 336–369 µs | + TCP/IP twice per side, c2cd hops |
| Remote GPU sync, old build | 326–352 µs | the TCP path + the GPU server |

Throughput was already at the physical limit in every path: USB 3.1 Gen 1 is 5 Gbit/s with 8b/10b
coding, so 500 MB/s raw, and about 470 MB/s after packet framing. TCP and raw both measured
465–470 MB/s per direction. **So TCP/IP cost latency (more than half the round trip) but no
bandwidth.** The bandwidth gains available in software were therefore:
1. use both directions at once (USB 3 is full duplex);
2. remove serialization inside our own GPU protocol.

## 2. What was built

1. **Raw USB data channels.** The slave's gadget now exposes up to 7 more USB interfaces next to
   the link interface, each with its own bulk endpoint pair.
   - The master's driver turns each one into `/dev/c2cchN` (mode 0666).
   - An application opens a channel, and its bytes go from its process, through the USB driver, to
     the slave's `c2c-gpud`. There is no TCP, IP, TUN or c2cd in between.
   - Opening a channel starts a session on the slave; closing it ends the session (the slave's
     `read()` returns `ECONNRESET`), like a TCP connect and close.
   - The kernel changes are confined to our two out-of-tree modules, so **the L4T image is not
     modified**.
2. **Busy-polling on both sides.** Each side spins for 1 ms after the last message before sleeping
   (configurable).
   - With a 200 µs window, half the replies arrived just after the spinner had gone to sleep:
     a bimodal p50 205 µs / p90 450 µs.
3. **A full-duplex GPU protocol.**
   - **Server:** the slave's server got a send thread with an ordered queue and a growing
     page-locked download pool (default 512 MB). Downloads stream out while the request thread
     keeps reading uploads and launches.
   - **Client:** `cuMemcpyDtoHAsync` into page-locked memory is now truly asynchronous. Its data
     arrives in stream order, before the reply of any later synchronising call. Pageable
     destinations stay synchronous, as in CUDA.
4. **No thread hand-off on the latency path.**
   - **Client (leader/follower):** a waiting caller reads its own reply; a background reader runs
     only while detached downloads are in flight.
   - **Server:** when the send queue is idle, small replies are written by the request thread
     itself.
   - A first version always handed off between threads, which added about 90 µs per call.
5. **Skipping redundant syncs.** cudart follows every pageable `cudaMemcpy` device-to-host with
   a `cuStreamSynchronize`. If nothing was sent since a completed synchronous download on that
   stream, the sync is answered locally, so a small read costs 1 round trip instead of 2.
6. **Pipelined GPU↔GPU staging.** Copies between the local and the remote GPU now go through four
   persistent 4 MiB page-locked buffers, with transfers overlapping.
   - Before: one 64 MiB pageable buffer that was zero-filled on every call, with no overlap.
7. **Combined small writes.** A small download's header and data go out as one write, which is
   one USB transfer.

## 3. Results (installed build, run 20261102-222047, T01–T13 all PASS)

| Remote GPU, same moment, same boards | Old (TCP) | **New (raw channel)** | Factor |
|---|---|---|---|
| sync round trip, p50 | 326–342 µs | **138–143 µs** | 2.4× |
| sync round trip, p99 | 406 µs | **170–178 µs** | 2.3× |
| 4-byte read (cudaMemcpy D2H), p50 | 376 µs | **160–167 µs** | 2.3× |
| launch + sync (gpu_check) | 352 µs | **156–163 µs** | 2.2× |
| upload / download | 459 / 462 MB/s | **471 / 460 MB/s** | line rate |
| **upload + download at once** | 464 MB/s total | **850–872 MB/s total** | **1.9×** |
| GPU0 → GPU1 / GPU1 → GPU0 | 422 / 434 MB/s | **466–469 / 465–466 MB/s** | line rate |
| p2pBandwidthLatencyTest, cross-board | 0.42–0.43 GB/s | **0.47 GB/s** | |
| async kernel launches | 257k/s | 246–255k/s | same |

- **TCP sessions benefit too.** The new server gives them duplex (761 MB/s total) and the sync
  skip, so a small read over TCP takes 367 µs instead of about 620 µs.
- **Compute is unchanged and native:** SGEMM 1.38 TFLOPS remote against 1.38 native.

NVIDIA's own tools agree:
- **bandwidthTest:** 0.5 GB/s each way (it rounds to 0.1).
- **nvbandwidth** copy-engine tests: 0.47 / 0.47 GB/s, and **0.47 + 0.50 GB/s** with both
  directions at once.
- **p2pBandwidthLatencyTest:** 0.47 GB/s each way.

## 4. Ruled out, and why

### Physical links on an AGX Xavier

| Option | Verdict | Evidence / reason |
|---|---|---|
| USB-C, USB 3.1 Gen 1 device mode (chosen) | **used** | 470 MB/s per direction, 144 µs user-space round trip |
| USB 3.1 Gen 2 (10 Gbit/s) | **impossible** | the Xavier's device controller (tegra-xudc) is Gen 1 only; the link negotiates 5 Gbit/s (`c2c doctor` checks it) |
| A second USB-C cable | **impossible** | only the flashing port (J512) can be a USB device; each extra link needs a device-capable port on one side |
| Gigabit Ethernet (RJ45) | **slower** | measured between the boards: **118 MB/s, 698 µs round trip** (through the LAN switch), so 4× less bandwidth and 5× more latency than raw USB. Striping it with USB could add at most about 25% to bulk transfers only. Not done: it would need TCP again and a second cable. |
| PCIe endpoint mode (Xavier's C5 controller, x8 Gen 4) | **excluded by the rules** | the only path to a much higher bandwidth class (GB/s), but it needs a different ODMDATA / device tree flashed into the L4T image and a PCIe crossover adapter. Out of scope for an ad-hoc design that must not patch the image. |
| NVLink / NVSwitch | **does not exist on Jetson** | the reason NCCL is unsupported here |

### Software

| Option | Verdict | Evidence / reason |
|---|---|---|
| TCP over TUN via c2cd (old path) | kept as **fallback** | 336–369 µs round trip, and it occasionally quantizes to exactly 1 or 2 ms (old client + old server too). Its one advantage: it survives a cable cut of a few seconds (T11). |
| A GPU channel multiplexed inside c2cd (shared memory) | rejected | still adds a daemon hop and a thread wake-up; separate USB interfaces cost nothing |
| In-kernel GPU server (no user space) | rejected | the floor is 90–120 µs against 144 µs for user space with busy-polling; it would need the CUDA driver inside the kernel |
| Larger or deeper USB requests | already tuned | 64 KiB × 32 requests per endpoint measured best on tegra xHCI ↔ xudc |
| USB 3 link power states (U1/U2) | off | +0.5 ms when idle (`allow_lpm=0`) |
| Remaining jitter | hardware | latencies sit on about 120 µs steps (160 / 250 / 380 µs…): the host re-polls a NAK'd endpoint on the 125 µs bus interval |
| Compression | rejected as default | GPU tensors and weights barely compress, and per-chunk CPU compression adds latency on the critical path (not measured here) |
| Zero-copy USB DMA from user pages | not needed | copying at 470 MB/s costs about 5% of one core |
| NCCL across the boards | **impossible** | NCCL kernels read and write host-mapped and peer memory directly (`cudaHostRegister` fails, and socket transport gives `CUDA_ERROR_ILLEGAL_ADDRESS`); a GPU on another board cannot address the master's RAM |
| Remote RAM over the raw channels | not done | page-in is bound by the kernel swap-in path (0.14–0.15 GiB/s against a 470 MB/s link); saving 0.2 ms per 512 KiB fault would gain about 10% and needs a userspace bridge for nbd |

### What was taken from NVIDIA's code
- **nvbandwidth:** copy-engine copies from page-locked memory, and measuring bidirectional
  throughput. That led to the duplex server and client, and the page-locked download pool.
- **NCCL / p2pBandwidthLatencyTest:** pipelining with several chunks in flight, which became the
  pipelined GPU↔GPU staging. p2pBandwidthLatencyTest also exposed the serialization (0.43 GB/s
  in both its unidirectional and bidirectional tests).
- **bandwidthTest:** persistent page-locked staging instead of pageable temporaries.

## 5. "One machine": what the OS can see (no kernel image changes)

| Resource | How | Limit |
|---|---|---|
| GPU | `c2c gpu system on` (config `gpu_system_wide=yes`): an `/etc/ld.so.conf.d` entry puts our libcuda front ahead of NVIDIA's, so **every unmodified CUDA program** sees the peer's GPU as the last device (deviceQuery: 1 → 2 devices). A session costs the slave nothing until the program uses the remote GPU (lazy CUDA start), and when the link is down programs start at once with the local GPU. | no unified memory, graphs or NCCL on the remote GPU |
| RAM | `rmem_lend_gib` (slave) + `rmem_auto=yes` (master): the peer's RAM becomes swap at boot (`free` shows it) | swap speed: 0.14–0.15 GiB/s page-in |
| CPU | `c2c cpu run CMD` and `c2c cpu pipe 'FILTER' FILES…`: jobs run on both nodes' cores (T03: 1.68× on 16 cores) | the scheduler cannot see remote CPUs without a kernel change (single-system-image kernels such as OpenMosix or Popcorn need patches), so offload works per job, not per thread |

## 6. Known issue seen during development
After many manual reloads of the gadget module with files still open (development only), the
slave's USB controller dropped master→slave throughput to 31 MB/s (TCP 0.33 Gbit/s, run
`20261102-220359` T01 FAIL), until the gadget was re-attached. It did not happen again in normal
use: sessions, killed clients, TCP sessions, NVIDIA benchmarks, live channel changes and a real
USB cut (T13) all leave the link at full speed. If it ever appears, `sudo systemctl restart
c2c-gadget` on the slave restores it, and `c2c doctor` / T01 / T12 detect it.
