# NVIDIA interconnect benchmarks over usbc-c2c

These are the standard tools NVIDIA uses to measure GPU interconnects
(NVLink / NVSwitch / PCIe). They run unmodified, through `c2c-gpu run`, with
the slave's GPU as CUDA device 1. They were run on 2026-11-02 with master
xavier (.166) and slave xavier32 (.173).

| tool | version | runs over C2C? |
|---|---|---|
| `bandwidthTest` (CUDA samples 11.4) | 11.4 | yes, unmodified |
| `p2pBandwidthLatencyTest` (CUDA samples 11.4) | 11.4 | yes, unmodified |
| `nvbandwidth` | v0.5 + 5-line patch | copy-engine tests: yes. SM-copy and latency tests: no (they need GPU access to host memory) |
| NCCL `nccl-tests` | NCCL 2.18.6, nccl-tests 2.13.9 | no: NCCL's kernels need direct GPU access to host or peer memory |

The full results are in `results/local` (master GPU only) and `results/remote`
(both GPUs). The summary is in `../../docs/TEST_LOG_2026-11-02_xavier173.md`.

## Adaptations (benchmark side only; the C2C system is unchanged)

- **`nvml_stub.c`**: Jetson has no NVML (`libnvidia-ml.so.1`). The stub
  answers the few NVML calls nvbandwidth and NCCL make, using the CUDA
  driver's device list. NVLink and NVML P2P report "not supported".
- **`nvbandwidth-v0.5-no-host-latch.patch`**: before each copy, nvbandwidth
  starts a 1-thread kernel that spins on a flag in pinned *host* memory, so
  that all copies start together. A GPU on another board cannot read the
  master's RAM; the remote GPU reports `canMapHostMemory = 0`. Without the
  patch, the kernel faults (`CUDA_ERROR_ILLEGAL_ADDRESS`,
  `results/remote/nvbandwidth_unpatched_FAIL.txt`). The patch skips the
  latch only on such devices. The copies and their timing are unchanged.
- **Version pins**: nvbandwidth ≥ v0.6 needs CUDA 12. nccl-tests 2.21 fails
  with NCCL 2.18 even on one local GPU, so 2.13.9 (from the NCCL 2.18 era) is
  used.

## Reproduce

```bash
bench/nvidia/build.sh                                   # on the master
export LD_LIBRARY_PATH=~/c2c-bench/nccl/build/lib:~/c2c-bench/nvml-stub
S=~/C2C-thunderbolt/tests/build/samples/1_Utilities     # built by tests/setup_tests.sh
c2c-gpu run $S/bandwidthTest/bandwidthTest --device=1 --memory=pinned
c2c-gpu run $S/p2pBandwidthLatencyTest/p2pBandwidthLatencyTest
c2c-gpu run ~/c2c-bench/nvbandwidth/build/nvbandwidth -b 64 -t 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
c2c-gpu run ~/c2c-bench/nccl-tests/build/all_reduce_perf -b 8 -e 64M -f 4 -g 2   # fails, see above
```
