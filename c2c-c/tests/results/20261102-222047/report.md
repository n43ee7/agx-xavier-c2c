# C2C proof report (2026-11-02 22:33)

**13/13 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.310 ms avg, TCP 3.719 Gbit/s master->slave, 3.718 Gbit/s slave->master |
| T02 cluster inventory (CPUs, RAM, GPUs) | **PASS** | 16 CPUs (8+8), 44.8 GiB RAM (14.54+30.26), 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores, peak 2.82 FP32 TFLOPS |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 43.22 s, pooled 16 cores 25.8 s = 1.68x; slave did 29/64 chunks; CPU mid-run master 99.4% slave 98.5% |
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.51 GiB), write 0.17 GiB/s, verify 0.138 GiB/s, 0 errors |
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.0 GiB held in slave RAM, zram 0.0 GiB, write 0.262 GiB/s, verify 0.147 GiB/s, 0 errors |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 30.0 GiB allocated (10.25+19.75), FP32 2.786 TFLOPS, SGEMM 2.779 TFLOPS, tensor-core HGEMM 20.552 TFLOPS, 231.44 GB/s mem bandwidth; slave GPU 90.9% peak load (c2cd telemetry) |
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.6 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |
| T08 remote GPU as a local CUDA device | **PASS** | master sees 2 GPUs (remote: Xavier 30.3 GiB); remote results verified=True; remote/native: SGEMM 1.382/1.383 TFLOPS, HGEMM 10.086/10.039, membw 111.43/112.66 GB/s, alloc 19.0 GiB; link 471.3/462.7 MB/s, 255034 launches/s, sync 156.3 us, via /dev/c2cch0: |
| T09 one process uses both GPUs | **PASS** | one process, 2 GPUs: split SAXPY over 268435456 elements on 2 GPUs ok=True (3.786 s); per-GPU checks [True, True]; samples {'simpleMultiGPU': 'PASS', 'MonteCarloMultiGPU': 'PASS', 'deviceQuery': 'PASS'}; deviceQuery: Detected 2 CUDA Capable device(s) |
| T10 NVIDIA CUDA samples on the remote GPU | **PASS** | 49 unmodified samples: remote 48 pass / 1 waived / 0 fail; local 49 pass / 0 waived / 0 fail; remote-only failures: none; waived remotely: ['UnifiedMemoryStreams'] |
| T11 remote GPU (TCP) survives link outage | **PASS** | remote GPU job ran through a 1.5 s link cut: exit 0, results verified=True, total 10.8 s, link UP |
| T12 raw USB channels (TCP-free GPU data path) | **PASS** | raw: up 470.8 / down 459.6 / both 871.8 MB/s, sync p50 140.0 us (TCP 341.6 us), 4B read p50 159.9 us (TCP 364.1), GPU0<->GPU1 468.5/465.8 MB/s; 5 programs at once: 4 raw + 1 TCP, all ok=True |
| T13 raw channel: clean failure and recovery after a USB cut | **PASS** | USB cut 1.5 s: running program ended in 0.0 s with exit 2 (no hang=True); link UP after 2.5 s; new raw session /dev/c2cch0:: up 470.4 / down 443.6 MB/s, sync p50 140.5 us |

## What the master can use

| | master | slave | total |
|---|---|---|---|
| CPU cores | 8 x NVIDIA Carmel (ARMv8.2) @ 2265 MHz | 8 | **16** |
| RAM | 14.54 GiB | 30.26 GiB | **44.8 GiB** |
| GPU | Xavier (cc 7.2) | Xavier | **2 GPUs** |
| SMs / CUDA cores / tensor cores | 8 / 512 / 64 | 8 / 512 / 64 | **16 / 1024 / 128** |
| GPU memory (unified) | 14.54 GiB | 30.26 GiB | **44.8 GiB** |
| peak FP32 | 1.41 TFLOPS | 1.41 TFLOPS | **2.82 TFLOPS** |
| NVENC / NVDEC / DLA | | | 2 / 4 / 4 |

## Raw metrics

### T01 link health (PASS, 2026-11-02 22:21:00)

```json
{
  "rtt_avg_ms": 0.31,
  "rtt_max_ms": 0.526,
  "rtt_mdev_ms": 0.025,
  "tcp_m2s_gbps": 3.719,
  "tcp_s2m_gbps": 3.718
}
```

### T02 cluster inventory (CPUs, RAM, GPUs) (PASS, 2026-11-02 22:21:02)

```json
{
  "master": {
    "host": "xavier",
    "cpus": 8,
    "cpu": "NVIDIA Carmel (ARMv8.2)",
    "cpu_max_mhz": 2265,
    "ram_gib": 14.54,
    "ram_avail_gib": 11.24,
    "gpu": "Xavier",
    "gpu_cc": "7.2",
    "gpu_sms": 8,
    "cuda_cores": 512,
    "tensor_cores": 64,
    "gpu_clock_mhz": 1377,
    "gpu_mem_gib": 14.54,
    "peak_fp32_tflops": 1.41
  },
  "slave": {
    "host": "xavier32",
    "cpus": 8,
    "cpu": "NVIDIA Carmel (ARMv8.2)",
    "cpu_max_mhz": 2265,
    "ram_gib": 30.26,
    "ram_avail_gib": 25.04,
    "gpu": "Xavier",
    "gpu_cc": "7.2",
    "gpu_sms": 8,
    "cuda_cores": 512,
    "tensor_cores": 64,
    "gpu_clock_mhz": 1377,
    "gpu_mem_gib": 30.26,
    "peak_fp32_tflops": 1.41
  },
  "cluster": {
    "nodes": 2,
    "cpus": 16,
    "ram_gib": 44.8,
    "ram_avail_gib": 36.28,
    "gpus": 2,
    "gpu_sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "gpu_mem_gib": 44.8,
    "peak_fp32_tflops": 2.82,
    "nvenc": 2,
    "nvdec": 4,
    "dla": 4
  },
  "telemetry_matches_probe": true
}
```

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-02 22:22:36)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 43.22,
  "pooled_s": 25.8,
  "speedup": 1.68,
  "master_only_mib_s": 11.8,
  "pooled_mib_s": 19.8,
  "chunks_done_by_slave": 29,
  "compressed_mib": 140.8,
  "link_tx_mib": 235.1,
  "link_rx_mib": 63.5,
  "cpu_util_mid_run": {
    "master": 99.4,
    "slave": 98.5
  },
  "verified": true
}
```

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 22:24:28)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 47.11,
  "verify_s": 58.12,
  "write_gib_s": 0.17,
  "verify_gib_s": 0.138,
  "errors": 0,
  "rss_after_write_kb": 2081368,
  "swap_after_write_kb": 6308212,
  "peak_rss_kb": 2089780,
  "peak_swap_kb": 6308432,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.51
}
```

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-02 22:28:18)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 78.26,
  "verify_s": 139.05,
  "write_gib_s": 0.262,
  "verify_gib_s": 0.147,
  "errors": 0,
  "rss_after_write_kb": 10545872,
  "swap_after_write_kb": 10951020,
  "peak_rss_kb": 12031348,
  "peak_swap_kb": 10951020,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 12.52,
  "footprint_over_physical_gib": 5.96
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-02 22:28:30)

```json
{
  "master": {
    "host": "xavier",
    "name": "Xavier",
    "max_alloc_gib": 10.25,
    "gpu_total_mem_gib": 14.54,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.396,
    "hgemm_tc_tflops": 10.367,
    "membw_gbs": 118.02
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 19.75,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.39,
    "sgemm_tflops": 1.383,
    "hgemm_tc_tflops": 10.185,
    "membw_gbs": 113.42
  },
  "cluster": {
    "fp32_fma_tflops": 2.786,
    "sgemm_tflops": 2.779,
    "hgemm_tc_tflops": 20.552,
    "membw_gbs": 231.44,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 30.0
  },
  "gpu_util_peak": {
    "master": 46.2,
    "slave": 90.9
  }
}
```

### T07 remote memory survives link outage (PASS, 2026-11-02 22:29:03)

```json
{
  "data_mib": 1024,
  "write_mib_s": 343.2,
  "link_outage_s": 1.56,
  "read_total_s": 5.81,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

### T08 remote GPU as a local CUDA device (PASS, 2026-11-02 22:29:35)

```json
{
  "gpus": [
    {
      "ordinal": 0,
      "name": "Xavier",
      "cc": "7.2",
      "sms": 8,
      "clock_mhz": 1377,
      "total_mem": 15609847808,
      "pci": "0000:00:00.0",
      "remote": false
    },
    {
      "ordinal": 1,
      "name": "Xavier",
      "cc": "7.2",
      "sms": 8,
      "clock_mhz": 1377,
      "total_mem": 32496168960,
      "pci": "0c2c:00:00.0",
      "remote": true
    }
  ],
  "checks": {
    "saxpy_ok": true,
    "reduction_ok": true,
    "sgemm_ok": true,
    "struct_param_ok": true,
    "streams_ok": true
  },
  "link": {
    "htod_pinned_mbs": 471.3,
    "dtoh_pinned_mbs": 462.7,
    "htod_pageable_mbs": 473.1,
    "dtoh_pageable_mbs": 425.7,
    "async_launches_per_s": 255034,
    "launch_sync_us": 156.3,
    "dtoh_4b_us": 164.9
  },
  "native_on_slave": {
    "fp32_fma_tflops": 1.39,
    "sgemm_tflops": 1.383,
    "hgemm_tc_tflops": 10.039,
    "membw_gbs": 112.66,
    "max_alloc": 20401094656
  },
  "remote_from_master": {
    "fp32_fma_tflops": 1.391,
    "sgemm_tflops": 1.382,
    "hgemm_tc_tflops": 10.086,
    "membw_gbs": 111.43,
    "max_alloc": 20401094656
  },
  "remote_vs_native": {
    "fp32_fma_tflops": 1.001,
    "sgemm_tflops": 0.999,
    "hgemm_tc_tflops": 1.005,
    "membw_gbs": 0.989
  },
  "transport": "/dev/c2cch0:"
}
```

### T09 one process uses both GPUs (PASS, 2026-11-02 22:29:59)

```json
{
  "gpus": 2,
  "split_saxpy": {
    "devices": 2,
    "elements": 268435456,
    "seconds": 3.786,
    "ok": true
  },
  "per_gpu": [
    {
      "device": 0,
      "pci_domain": 0,
      "ok": true,
      "saxpy_ok": true,
      "sgemm_ok": true,
      "streams_ok": true,
      "htod_pinned_mbs": 37318.5,
      "async_launches_per_s": 170498
    },
    {
      "device": 1,
      "pci_domain": 3116,
      "ok": true,
      "saxpy_ok": true,
      "sgemm_ok": true,
      "streams_ok": true,
      "htod_pinned_mbs": 471.3,
      "async_launches_per_s": 254200
    }
  ],
  "samples": {
    "simpleMultiGPU": "PASS",
    "MonteCarloMultiGPU": "PASS",
    "deviceQuery": "PASS"
  },
  "deviceQuery": "Detected 2 CUDA Capable device(s)"
}
```

### T10 NVIDIA CUDA samples on the remote GPU (PASS, 2026-11-02 22:32:44)

```json
{
  "samples": 49,
  "remote": {
    "PASS": 48,
    "WAIVED": 1,
    "FAIL": 0
  },
  "local": {
    "PASS": 49,
    "WAIVED": 0,
    "FAIL": 0
  },
  "remote_failures_that_pass_locally": [],
  "local_failures": [],
  "waived_remote": [
    "UnifiedMemoryStreams"
  ],
  "per_sample": {
    "BlackScholes": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.86,
      "local_s": 1.45
    },
    "MersenneTwisterGP11213": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.44,
      "local_s": 0.3
    },
    "MonteCarloMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.45,
      "local_s": 0.29
    },
    "SobolQRNG": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.51,
      "local_s": 0.3
    },
    "UnifiedMemoryStreams": {
      "local": "PASS",
      "remote": "WAIVED",
      "remote_s": 0.35,
      "local_s": 2.88
    },
    "asyncAPI": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.87,
      "local_s": 0.43
    },
    "bandwidthTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 14.56,
      "local_s": 0.82
    },
    "batchCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.97,
      "local_s": 2.28
    },
    "binomialOptions": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 4.4,
      "local_s": 4.21
    },
    "clock": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.18
    },
    "concurrentKernels": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.19
    },
    "conjugateGradient": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.42,
      "local_s": 1.16
    },
    "convolutionSeparable": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.51,
      "local_s": 2.62
    },
    "cppIntegration": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.18
    },
    "dct8x8": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.76,
      "local_s": 0.62
    },
    "deviceQuery": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.06
    },
    "deviceQueryDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.09,
      "local_s": 0.06
    },
    "dwtHaar1D": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.2
    },
    "fastWalshTransform": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.52,
      "local_s": 2.28
    },
    "histogram": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.56,
      "local_s": 1.31
    },
    "inlinePTX": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.19
    },
    "matrixMul": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.47,
      "local_s": 0.3
    },
    "matrixMulDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.19
    },
    "mergeSort": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.67,
      "local_s": 0.42
    },
    "p2pBandwidthLatencyTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.53,
      "local_s": 0.35
    },
    "quasirandomGenerator": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.16,
      "local_s": 0.99
    },
    "reduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.88,
      "local_s": 0.58
    },
    "scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.97,
      "local_s": 1.77
    },
    "shfl_scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.4,
      "local_s": 0.22
    },
    "simpleAWBarrier": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.88,
      "local_s": 0.5
    },
    "simpleAtomicIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "simpleCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.0,
      "local_s": 2.33
    },
    "simpleCUBLASXT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 13.37,
      "local_s": 12.45
    },
    "simpleCUFFT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.97,
      "local_s": 0.44
    },
    "simpleCallback": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.45,
      "local_s": 0.19
    },
    "simpleCooperativeGroups": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.19
    },
    "simpleMultiCopy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.14,
      "local_s": 0.76
    },
    "simpleMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.74,
      "local_s": 1.41
    },
    "simpleOccupancy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.37,
      "local_s": 0.22
    },
    "simpleStreams": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 29.58,
      "local_s": 1.17
    },
    "simpleTemplates": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.18
    },
    "simpleTexture": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.37,
      "local_s": 0.23
    },
    "simpleVoteIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.18
    },
    "simpleZeroCopy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.27
    },
    "sortingNetworks": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.66,
      "local_s": 3.14
    },
    "threadFenceReduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.41,
      "local_s": 0.24
    },
    "transpose": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.65,
      "local_s": 0.37
    },
    "vectorAdd": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.19
    },
    "vectorAddDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.34,
      "local_s": 0.2
    }
  }
}
```

### T11 remote GPU (TCP) survives link outage (PASS, 2026-11-02 22:32:55)

```json
{
  "exit_code": 0,
  "link_outage_s": 1.52,
  "run_s": 10.77,
  "link_state_after": "UP",
  "checks": {
    "saxpy_ok": true,
    "reduction_ok": true,
    "sgemm_ok": true,
    "struct_param_ok": true,
    "streams_ok": true
  },
  "ran_on_remote": true
}
```

### T12 raw USB channels (TCP-free GPU data path) (PASS, 2026-11-02 22:33:12)

```json
{
  "raw": {
    "h2d_mbs": 470.8,
    "d2h_mbs": 459.6,
    "bidir_mbs": 871.8,
    "sync_p50_us": 140.0,
    "sync_p99_us": 175.1,
    "read4_p50_us": 159.9,
    "peer_to_mbs": 468.5,
    "peer_from_mbs": 465.8
  },
  "tcp": {
    "h2d_mbs": 458.2,
    "d2h_mbs": 459.3,
    "bidir_mbs": 853.0,
    "sync_p50_us": 341.6,
    "sync_p99_us": 419.1,
    "read4_p50_us": 364.1,
    "peer_to_mbs": 464.4,
    "peer_from_mbs": 464.2
  },
  "channels": 4,
  "concurrent": {
    "programs": 5,
    "raw": 4,
    "tcp": 1,
    "all_ok": true,
    "h2d_mbs": [
      93.8,
      93.5,
      94.1,
      92.7,
      93.8
    ]
  }
}
```

### T13 raw channel: clean failure and recovery after a USB cut (PASS, 2026-11-02 22:33:26)

```json
{
  "first_program_exit": 2,
  "ended_after_cut_s": 0.0,
  "no_hang": true,
  "link_state": "UP",
  "link_back_s": 2.5,
  "after": {
    "h2d_mbs": 470.4,
    "d2h_mbs": 443.6,
    "bidir_mbs": 850.5,
    "sync_p50_us": 140.5
  },
  "after_via": "/dev/c2cch0:"
}
```

