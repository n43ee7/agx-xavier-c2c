# Raw USB channels, full-duplex GPU path, packaging (2026-11-02, night)

Branch `feature/usbc-raw-channels`; master xavier (.166), slave xavier32 (.173).
Design, comparisons and ruled-out alternatives are in
[DESIGN_AND_ALTERNATIVES.md](DESIGN_AND_ALTERNATIVES.md).

## Final result: 13/13 PASS (run 20261102-222047, installed build)

| test | result |
|---|---|
| T01 link health | RTT 0.310 ms, TCP 3.719 / 3.718 Gbit/s (unchanged with 4 extra USB interfaces) |
| T02 inventory | 16 CPUs, 44.8 GiB, 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores |
| T03 CPU pool | xz 43.22 s → 25.8 s on 16 cores (1.68×) |
| T04 remote paging | 8 GiB in slave RAM, page-in 0.138 GiB/s, 0 errors |
| T05 beyond physical | 20.5 GiB on the 14.5 GiB master, page-in 0.147 GiB/s, 0 errors |
| T06 GPU pool | FP32 2.786, SGEMM 2.779, HGEMM 20.55 TFLOPS, 231 GB/s |
| T07 RAM link cut | 1.6 s cut mid-read, data identical |
| T08 remote GPU | SGEMM 1.382/1.383, HGEMM 10.09/10.04 TFLOPS; 471/463 MB/s; 255k launches/s; **sync 156 µs via /dev/c2cch0** (was 352–354 µs) |
| T09 two GPUs | split SAXPY verified; multi-GPU samples pass |
| T10 samples | remote 48 pass / 1 waived / 0 fail; local 49/49 |
| T11 GPU link cut (TCP) | job over TCP ran through a 1.5 s cut, verified |
| **T12 raw channels** | up 470.8 / down 459.6 / **both 871.8 MB/s**; sync p50 **140.0 µs** (TCP 341.6); 4-byte read p50 159.9 µs (TCP 364.1); GPU0↔GPU1 468.5 / 465.8 MB/s; 5 programs at once: 4 raw + 1 TCP, all OK |
| **T13 USB cut, raw** | real 1.5 s USB cut: running program ended immediately with an error (no hang); link UP after 2.5 s; new raw session 470.4 / 443.6 MB/s, sync 140.5 µs |

Re-run after the last client change (run 20261102-223634): T08, T12 and T13
PASS, with T12 at sync p50 137.9 µs and both directions 871.2 MB/s.

## Path to the result (measurements in order)

1. **User-space floor over the raw device** (`tools/rawlink/rawpp`, c2cd
   stopped): 201 µs blocking and 160 µs busy-polled, against 336 µs for TCP.
   Throughput was 470 / 466 MB/s on both paths. TCP/IP therefore costs
   latency, not bandwidth.
2. **Multi-interface gadget and host driver:** `/dev/c2cch0..3`. A channel
   measured 167 µs and 470 / 466 MB/s from an unprivileged user, and T01 was
   unchanged.
3. **First GPU path over a channel:** sync 188 µs, but TCP regressed to
   405–445 µs because every reply went through a thread hand-off.
   Leader/follower reading on the client and inline small replies on the
   server fixed it.
4. **4-byte read cost 2 round trips** in every version (cudart issues a sync
   after the device-to-host copy). Sync skipping fixed it: TCP 620 → 370 µs,
   raw → 160 µs.
5. **Raw latency was bimodal** (p50 205 µs, p90 450 µs) with 200 µs spin
   windows. Raising them to 1 ms fixed it. Latencies sit on 120–125 µs steps,
   the USB bus interval.
6. **Uploads at 0.03 GB/s in nvbandwidth** came from two separate problems:
   - a real bug: `REAL(cuMemcpyDtoH)` stringified to NVIDIA's legacy v1
     symbol (32-bit pointers) in the new staging code, fixed with `_v2`;
   - a degraded slave USB controller (see below).
7. **Both directions at once:** 807 MB/s at 64 MB, but 537 MB/s at 256 MB,
   because the server's 64 MB download pool blocked the request side. A
   growing pool (512 MB default) took it to **872 MB/s** (93% of 2 × 470).
8. **nvbandwidth** (copy-engine tests) gave 0.47 / 0.47 GB/s, and 0.47 + 0.50
   GB/s with both directions at once. **p2pBandwidthLatencyTest** gave
   0.47 GB/s each way (old 0.42–0.43); latency GPU1→GPU0 527 µs (old 638–778).

## Degraded controller (development only)
After many manual module reloads with channel files still open, the slave's
xudc dropped master→slave throughput to 31 MB/s. T01 FAILED in run
`20261102-220359` (TCP 0.33 Gbit/s); the old client against the old server
was slow too. Re-attaching the gadget restored 470 MB/s.

It could not be reproduced afterwards, by:
- GPU server start/stop;
- latency sessions;
- bulk sessions over raw and TCP;
- a client killed mid-upload;
- nvbandwidth and p2p runs;
- live `raw_channels` changes;
- T13's real USB cut.

`c2c doctor` and T01 / T12 detect it, and `systemctl restart c2c-gadget`
(slave) fixes it.

## Features tested on the boards
- **`c2c gpu system on`:** a plain `deviceQuery` (no wrapper) went from 1 to
  2 devices; `off` took it back to 1.
- **`c2c config`:** `set gpu_transport bogus` was refused (allowed:
  auto,raw,tcp). `rmem_lend_gib 8 --apply` (slave) plus `rmem_auto yes
  --apply` (master) attached 8 GiB of the slave's RAM as swap at priority
  100. Reverting detached it and restored `vm.page-cluster` to 3.
- **`raw_channels 2 --apply`** (slave): the master saw 2 channels and the
  link stayed at full speed. Setting 4 again restored 4.
- **Ethernet** between the boards, for comparison: 117.9 MB/s, 698 µs round
  trip.
- **CPU offload** (after the user approved `c2c cpu setup`): one key line
  on the slave (`from="10.77.0.1"`, link only); `c2c cpu run` worked ("on
  xavier32: 8 cores"). `c2c cpu pipe 'xz -6 -T1'` over 32 × 16 MiB files:
  48.4 s on the master's 8 cores, 32.7 s on 8 local + 8 remote cores
  (**1.48×**; the peer did 12 of 32 files), all 32 outputs verified bit for
  bit. T03 gets 1.68× with 64 smaller chunks: finer work units balance the
  tail better.
