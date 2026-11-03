# C2C-thunderbolt: work history and handoff

Written 2026-11-02, when xavier32 (the slave) was decommissioned. Read this
first when the work resumes on a new slave device. The detailed numbers are
in `TEST_LOG_2026-11-01.md` and `TEST_LOG_2026-11-02.md`. Design and usage
are in `../README.md` and `../gpu/README.md`.

## 00. Update 2026-11-02 night: raw USB channels + packaging (branch feature/usbc-raw-channels, merged)

- The TCP bypass is done. The remote GPU now runs over raw USB channels
  (`/dev/c2cchN`): sync 138 µs (was 352), upload + download at once 872 MB/s
  (was 464), GPU0↔GPU1 at line rate. T01–T13: 13/13 PASS. See
  `DESIGN_AND_ALTERNATIVES.md` and `TEST_LOG_2026-11-02_raw_channels.md`.
- Packaged: the `c2c` CLI, the `setup.sh` wizard (can set up the peer over
  SSH), every setting in `/etc/c2c/c2c.conf` (`c2c config show`), and the
  services c2c-clocks, c2c-memd and c2c-rmem.
- Bonus "one machine": system-wide GPU via ld.so (`c2c gpu system on`),
  remote RAM as swap at boot, CPU offload by job (`c2c cpu`). The scheduler
  cannot see the remote CPUs without kernel patches.
- Next:
  1. raw-channel session resume across USB cuts (sequence numbers plus a
     replay buffer on both sides);
  2. remote RAM over raw channels (about +10%);
  3. an NCCL net plugin, so NCCL works across the boards through a proxy
     that copies;
  4. if the rules ever allow flashing: PCIe endpoint mode, the only route to
     GB/s.

## 0. Update 2026-11-02 evening: new slave at 192.168.4.173

xavier32 (.171) was replaced by a new AGX Xavier 32 GB at **192.168.4.173**
(hostname also `xavier32`, L4T R35.6.4, JetPack 5.1.6). The link is paired,
and T01-T11 pass 11/11 with numbers matching or beating the old slave.
NVIDIA's own benchmarks (bandwidthTest, p2pBandwidthLatencyTest, nvbandwidth)
confirm our bandwidth numbers. NCCL cannot run (no GPU access to host or peer
memory). See `TEST_LOG_2026-11-02_xavier173.md` and `../bench/nvidia/`.

New lessons:
- **USB-C roles are set at plug-in**, and software cannot swap them
  (`ccgx-ucsi` data_role is read-only). If the master comes up as the
  device, replug and flip the cable at the slave end.
- **A new slave needs jtop's `jetson_clocks` at boot** (copy
  `/usr/local/jtop/config.json` from the master). On default clocks, launch
  rate halves and RTT rises from 0.31 to 0.54 ms.
- Throughput targets found by nvbandwidth / p2pBandwidthLatencyTest:
  - the link is full duplex for GPU copies (0.47 + 0.46 GB/s at once);
  - cross-board GPU↔GPU copies are staged serially (0.43 GB/s even when
    bidirectional), so pipelining them is the first win;
  - a TCP bypass cuts the latency.

## 1. Where things stand

| Area | State | Final numbers (run 20261102-062435, 11/11 PASS) |
|---|---|---|
| Keyed USB-C link (c2cd, kernel modules) | done, boot service | TCP 3.71 / 3.72 Gbit/s, 6.89 Gbit/s duplex, RTT 0.32 ms |
| CPU pool (T03) | done | `xz` 45.5 s → 27.1 s on 16 cores (1.68×) |
| Remote RAM (c2c-memd / c2c-rmem, nbd swap) | done | page-out 0.18–0.35 GiB/s, page-in 0.144–0.149 GiB/s |
| GPU pool telemetry (T06) | done | both GPUs 2.79 TFLOPS FP32, 20.5 TFLOPS HGEMM |
| Remote GPU as CUDA device 1 (gpu/) | done | native compute (SGEMM 1.387 vs 1.388), copies 468/460 MB/s, 257k launches/s, sync 352 µs; 48/49 NVIDIA samples |
| Link-outage resilience | done for cuts under the 3 s heartbeat timeout | T07 (RAM) and T11 (GPU) survive 1.5 s cuts |
| TCP bypass for the GPU data plane | **done 2026-11-02 night** (raw USB channels) | sync 138 µs, both directions 872 MB/s |

Every test run on 2026-11-01 and 2026-11-02 is in `tests/results/` (29
directories). All the runs on 11-02 passed.

## 2. Timeline

**2026-11-01**
1. **Raw USB benchmark** (`tools/test_bw_spd`): the ceiling is 449.5 MiB/s. Stock NCM managed about 0.55 Gbit/s.
2. **Kernel modules:**
   - `usb_f_c2c` gadget on the slave, `c2c_host` on the master.
   - Exclusive gadget mode, because composite mode dropped throughput to about 45 MiB/s.
   - USB3 LPM disabled, since it added about 0.5 ms of RTT.
3. **c2cd daemon:**
   - Keyed HMAC handshake, heartbeat and telemetry.
   - TUN interface `c2c0` with vnet-header checksum offload, MTU 65000, TX aggregation.
   - Result: 3.72 Gbit/s TCP.
4. **Remote RAM:** c2c-memd on the slave and c2c-rmem on the master, attached as nbd swap.
5. **L4T 5.10 kernel bug** (kthreadd has no struct kthread, so swapping to nbd hits a NULL dereference in `kthread_should_stop`) panicked the master once. It is fixed at runtime by `kernel/kfix/c2c_kfix.ko`, and c2c-rmem refuses to swap without it.
6. **Tests T01–T07:** final run 7/7 PASS.

**2026-11-02**
1. **Remote GPU** (`gpu/`): `libc2ccuda` stands in for libcuda on the master and forwards to `c2c-gpud` on the slave.
   - Shadow contexts on the identical local GPU.
   - Shared VMM address window.
   - Kernel parameter layouts parsed from fatbins.
   - Async batching.
   - Module cache on the slave.
   - Arrays, textures and surfaces, and cooperative launch.
2. **ELPG finding:** Jetson GPU power gating added about 250 µs to every synchronous call. c2c-gpud turns it off while clients are connected and restores it 5 s after the last one leaves.
3. **Remote RAM page-in:** went from 0.083 to 0.149 GiB/s. The fix was page-cluster 7 plus cluster read-ahead (`vma_ra_enabled=false`). c2c-rmem now saves these settings when attaching and restores them when detaching.
4. **Master GPU telemetry:** fixed with a 20 ms sampler thread, so the reading went from 0.8% to 46.6%.
5. **c2cd busy-poll** (`busy_poll_us=200`): TCP request/response went from 369 to 336 µs.
6. **Tests T08–T11 added:** final run 11/11 PASS.

## 3. Lessons that cost the most time (don't relearn them)

**Link / kernel**
- **tegra-xudc rules:** see README "Design": never use `ep->driver_data`, never requeue from an error completion, never cancel on a mode switch, and flush stale reads.
- **Socket buffers:** don't set SO_SNDBUF/SO_RCVBUF on link sockets. It disables autotuning and caps the link at about 165 MB/s.
- **Before raw benchmarks:** stop c2cd on the master before running `test_bw_spd`.
- **After a watchdog reboot:** crash logs are in `/sys/fs/pstore/dmesg-ramoops-*`.

**Remote GPU** (`gpu/libc2ccuda.cpp`)
- **cudart lookups:** cudart 11.4 gets almost every driver function through `cuGetProcAddress`. That call must return *our* `cuGetProcAddress` and `cuGetExportTable` too, or cudart ends up calling the real driver.
- **Hidden export table** `6bd5fb6c-5bf4-e74a-8987-d93912fd9df9`:
  - slot 2 returns the primary context; for the remote ordinal it must return the shadow context, or you get error 709;
  - slot 6 loads registered fatbins; hook it to register modules.
- **Device addresses:** both drivers give out identical addresses (0x203…). Master and slave therefore share one `cuMemAddressReserve` window at 0x991000000, sized total + 4 GiB.
  - Reserve it **before** a second context exists, or you get error 208.
  - The side with the lower VMM start pads up to the other side's start with a filler reservation.
- **Kernel parameter layouts:**
  - From cubin `.nv.info`: EIATTR_KPARAM_INFO (0x17), with size = (bits>>18)&0x3fff, plus EIATTR_CBANK_PARAM_SIZE (0x19) and PARAM_CBANK (0x0a).
  - Fallback: PTX `.entry`.
  - Fatbin flag 0x2000 means LZ4-style compression. The parser was checked against `cuobjdump` on all 3,879 sm_72 kernels in cuBLAS.
- **`cuda.h` versioning:** it maps the primary-context functions to `_v2`. Define only the `_v2` versions.
- **Generated trampolines:** `gen_tramp.py` must also match the MEMSET macros.
- **Launch rate:** the server needs a buffered reader, and the client batches async calls and flushes them at sync calls, at 256 KB, or every 100 µs. This took the launch rate from 29.5k/s to 257k/s.

**Remote RAM**
- On 5.10, per-VMA read-ahead is capped at 32 pages. Use cluster read-ahead (`vma_ra_enabled=false`) with page-cluster 7, which gives 512 KiB per fault.

**Process**
- **Variance:** T05 write speed varies 0.28–0.42 GiB/s from run to run. Don't chase a single run.
- **Constraint:** never damage either machine's OS or files. Every runtime change (sysctls, ELPG) is saved and restored.

## 4. Next task: TCP bypass for the GPU data plane

**Why.** A remote GPU sync takes 352 µs, while raw USB ping-pong takes 90–120 µs. Breakdown of that gap:
- about 230 µs is TCP and TUN, traversed twice on each side;
- c2cd user-space hops add more on top.

Throughput (465 MB/s) is already at the link's ceiling. Only latency gains.

**Plan sketch**
1. **Dedicated channel.** Add a framed channel (for example CH_GPU) to the c2cd wire protocol, next to CTRL and NET.
   - Or give the GPU path its own USB endpoint pair or a second `/dev/c2c*` minor, with no daemon in between.
2. **Transport abstraction.** Have `libc2ccuda` and `c2c-gpud` talk through a transport interface: the TCP transport today, plus a "direct" transport that reads and writes the char device (or a shared-memory ring served by c2cd).
   - The message format in `gpu/gpu_proto.hpp` stays the same.
3. **Kernel side.**
   - The host driver already has a ping-pong ioctl that hits 90–120 µs; reuse its path.
   - Busy-poll for a short window after each request, as c2cd's `busy_poll_us` does.
4. **Measure with the existing tools:**
   - T08 (`sync_us`, launches/s, copy MB/s);
   - `tools/dev/rr.py` (TCP request/response);
   - `test_bw_spd` ping-pong;
   - `tools/dev/idle.cu` (checks that ELPG is off).
5. **Keep TCP as the fallback.** It is what makes T11 (surviving a link cut) work today.

## 5. Bringing up a new slave device

- **Same GPU (another Xavier, sm_72):** everything should work as is. Run `setup_master.sh` on the master, then `setup_slave.sh --key <KEY>` on the slave. Then:
  - `tests/setup_tests.sh` (it builds the NVIDIA samples, SMS=72);
  - `sudo C2C_PEER_PASS=<pw> tests/run_all.sh`.
- **Different GPU** (Orin = sm_87, or a dGPU):
  - The shadow-context design assumes the local and remote GPU are identical: queries, occupancy and module bookkeeping are answered by the *local* GPU. That needs rework. Either answer device attributes and occupancy from the remote side, or keep a per-architecture shadow.
  - The fatbin path sends only the image for the slave's architecture, so it needs that architecture's SASS or PTX in the binary.
- **Different kernel or L4T:**
  - Re-check the tegra-xudc rules for the gadget module.
  - `c2c_kfix` is specific to the L4T 5.10 bug: check whether the new kernel still has the bug before relying on it.
  - The `vma_ra_enabled` and page-cluster behaviour may differ.
- **ELPG:** the sysfs path is `/sys/devices/gpu.0/elpg_enable` on Xavier. On other Jetsons the path may differ, so check it. The `c2c-gpud --keep-power-gating` flag skips this step.

## 6. Archived state of the two Xaviers

`docs/devices/xavier32-slave/` and `docs/devices/xavier-master/` hold the following, captured before decommissioning:
- `system.txt`: L4T R35.6.5, kernel 5.10.216-tegra, CUDA 11.4.19;
- `c2c.conf`;
- the build logs;
- the systemd units;
- the c2cd / c2c-gpud journal.

The link key (`/etc/c2c/c2c.key`) is **not** in the repo on purpose; `*.key` is in `.gitignore`. A new pairing generates a new key.

## 7. Developer tools (`tools/dev/`)

| file | purpose |
|---|---|
| `rr.py` | TCP request/response latency over c2c0 (`srv` on the slave, client on the master) |
| `idle.cu` | launch + sync latency after idle gaps (shows the ELPG wake cost) |
| `va.cu` | probes where the driver places VMM reservations |
| `fatscan.py` | statistics on fatbin entries (architectures, compression) in a library |
| `cmp.py` | compares `gpu/fatdump` with `cuobjdump -elf` KPARAM_INFO |
| `build_samples.sh` | builds the NVIDIA CUDA samples for sm_72 |
| `cuda_trace/` | LD_PRELOAD tracer for every libcuda entry point (`gen.py` reads `syms.txt` and generates `tramp.S`; build together with `trace.c`). It showed which driver calls cudart and cuBLAS make. |
