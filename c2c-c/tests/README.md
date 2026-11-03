# C2C proof tests

These tests show that the master can use the slave's **CPUs, RAM and GPU**
through the C2C link with **unmodified programs**. Each workload is an
ordinary program that knows nothing about C2C: `xz`, a plain memory hog
(`mem_proof`) and a plain CUDA/cuBLAS program (`gpu_probe`). C2C supplies the
link, the resources behind it and the telemetry.

| test | proves | pass criterion |
|---|---|---|
| **T01_link_health** | handshake, latency, throughput of the link everything else uses | UP, RTT < 1 ms, ≥ 3 Gbit/s each way |
| **T02_cluster_inventory** | the master sees the slave's hardware (CPUs, RAM, GPU/SM/cores, NVENC/NVDEC/DLA); c2cd telemetry agrees with a direct probe | 2 GPUs, telemetry matches |
| **T03_cpu_pool** | one batch job (unmodified `xz`) on 8 master cores, then on 16 pooled cores, with the slave's input and output streamed over C2C; output verified bit-for-bit | verified, ≥ 1.6× faster |
| **T04_ram_remote_paging** | a program capped at 2 GiB RAM works on 8 GiB; the rest lives in the slave's RAM (c2c-rmem) and is verified | 0 errors, ≥ 4 GiB in slave RAM |
| **T05_ram_beyond_physical** | **one process uses more memory than the master physically has** (master RAM + 6 GiB); the overflow lives in the slave's RAM; every byte verified | footprint > physical RAM, 0 errors, overflow went to the slave and not to zram |
| **T06_gpu_pool** | the same CUDA program on both GPUs at once: largest usable allocation, FP32, cuBLAS SGEMM, tensor-core HGEMM, memory bandwidth, aggregated | both GPUs complete, combined allocation > either alone |
| **T07_rmem_link_blip** | 1 GiB of data in the slave's RAM; the link is cut (c2cd restart) in the middle of reading it back; the read stalls, resumes and returns identical data; the kernel fix `c2c_kfix` is loaded | identical data, still attached, link UP, kfix loaded |
| **T08_gpu_remote_device** | **the slave's GPU is a CUDA device of the master** (`c2c-gpu run`): 2 devices visible; `gpu_check` verifies kernels, atomics, `__device__` symbols, by-value structs, cuBLAS vs a CPU reference and a 4-stream pipeline on the remote GPU; `gpu_probe --bench` remote from the master vs natively on the slave | all results correct, remote compute ≥ 95% of native |
| **T09_gpu_one_process_two_gpus** | one unmodified process uses both GPUs at once: SAXPY over 256 Mi elements split across both, per-GPU checks, NVIDIA `simpleMultiGPU` / `MonteCarloMultiGPU` / `deviceQuery` | all correct, 2 devices |
| **T10_cuda_samples** | ~50 unmodified NVIDIA CUDA samples (runtime + driver API, cuBLAS, cuFFT, cuRAND, cuSPARSE, textures, cooperative groups, streams, events) on the remote GPU, and locally through the same shim | no sample fails remotely that passes locally; local unaffected |
| **T11_gpu_link_blip** | a remote-GPU job (`gpu_check`) over TCP runs while the link is cut (c2cd restart) | job finishes, every result correct |
| **T12_raw_channels** | `c2c-gpu-bench` over a raw USB channel vs TCP; channels+1 programs at once | up/down ≥ 440 MB/s, both at once ≥ 750 MB/s, raw sync p50 ≤ 70% of TCP, GPU0↔GPU1 ≥ 400 MB/s, N raw + 1 TCP all OK |
| **T13_raw_usb_cut** | a real 1.5 s USB cut (device de-authorized) during a raw-channel GPU job | the job ends with an error within 30 s (no hang), link back UP, a new raw session at ≥ 440 MB/s |

## Run (on the master)

```bash
sudo C2C_PEER_PASS=<slave password> tests/setup_tests.sh   # once: builds workloads on both nodes
sudo C2C_PEER_PASS=<slave password> tests/run_all.sh       # all tests  (or: run_all.sh T02 T05)
sudo tests/cleanup_tests.sh                                # removes everything the tests put on the nodes
```

Without `C2C_PEER_PASS` you are asked for the slave password once per run.
`setup_tests.sh --with-key` instead installs a dedicated SSH key on the slave
(tagged `c2c-test` and accepted only from 10.77.0.1). After that, no password
is needed; `cleanup_tests.sh` removes the key. All test traffic to the slave
goes through `c2c-slave` (10.77.0.2), so it runs over the link and never over
the LAN.

Results go in `tests/results/<timestamp>/`, with `results/latest` pointing to
the newest run:
- `report.md`: the summary table, the cluster resource table and the raw metrics;
- `summary.json`;
- one `T*.json` per test;
- `T04/T05.samples`: remote swap, zram and slave-resident memory, sampled once a second.

Knobs (environment): `C2C_T03_MB` (dataset size, 512), `C2C_T04_GIB` / `C2C_T04_CAP`
(8 / 2G), `C2C_T05_EXTRA_GIB` (6), `C2C_T05_LEND` (16G),
`C2C_RMEM_PAGE_CLUSTER` (swap readahead 2^N pages, 7), `C2C_RMEM_CONNS` (2).

T08–T13 need the remote GPU installed (`c2c-gpu` on the master, `c2c-gpud`
service on the slave; both come with `setup_master.sh` / `setup_slave.sh`).
`setup_tests.sh` also builds ~50 NVIDIA CUDA samples from
`/usr/local/cuda/samples` (unmodified, `SMS=72`) into `tests/build/samples`.

## How the RAM tests work (c2c-rmem)

```
master: program ── page fault / reclaim ── swap (prio 100) ── /dev/nbd0 ── TCP ── c2c0 ═USB═► slave: c2c-memd (RAM)
                                           zram (prio 5) stays as a fallback only
```

`c2c-memd` on the slave lends RAM as a block device: lazily allocated, and
returned on TRIM or detach. `c2c-rmem attach -w 100` on the master hands
the socket to the kernel NBD client and enables it as swap, at a higher
priority than L4T's zram. For as long as remote swap is attached, c2c-rmem
itself (not the tests) also changes these kernel settings, saving the old
values and restoring them at detach:
- `vm.page-cluster=7` and `vma_ra_enabled=false`: each page fault reads
  512 KiB instead of 32 KiB. The kernel's per-VMA readahead is capped at
  128 KiB, but the cluster readahead it falls back to is not;
- `vm.min_free_kbytes` ≥ 256 MiB, so the network path never starves;
- `vm.watermark_scale_factor` ≥ 200, so kswapd, not arbitrary tasks, does
  the swapping. That is defense in depth next to the `c2c_kfix` module,
  which fixes an L4T 5.10 kernel panic in direct reclaim (see the main
  README). c2cd is memory-locked, marked
`PR_SET_IO_FLUSHER` and OOM-exempt, so the link can never wait on the swap it
carries. Every test detaches in a trap, even on failure.

Measured on this pair: the raw remote block device runs at 400 MB/s write
and 412 MB/s read. Through the kernel swap path, page-out runs at
0.3–0.4 GiB/s. Page-in runs at 0.15 GiB/s (2026-11-02) with the 512 KiB
readahead, up from 0.08 GiB/s at 32 KiB. Page-in waits on a network round
trip per fault, so remote RAM adds memory, not speed.
