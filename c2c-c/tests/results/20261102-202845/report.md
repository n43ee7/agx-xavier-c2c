# C2C proof report (2026-11-02 20:40)

**11/11 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.309 ms avg, TCP 3.711 Gbit/s master->slave, 3.718 Gbit/s slave->master |
| T02 cluster inventory (CPUs, RAM, GPUs) | **PASS** | 16 CPUs (8+8), 44.8 GiB RAM (14.54+30.26), 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores, peak 2.82 FP32 TFLOPS |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 44.96 s, pooled 16 cores 26.93 s = 1.67x; slave did 32/64 chunks; CPU mid-run master 99.3% slave 99.7% |
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.51 GiB), write 0.167 GiB/s, verify 0.138 GiB/s, 0 errors |
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.33 GiB held in slave RAM, zram 0.0 GiB, write 0.302 GiB/s, verify 0.145 GiB/s, 0 errors |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 29.5 GiB allocated (10.25+19.25), FP32 2.787 TFLOPS, SGEMM 2.778 TFLOPS, tensor-core HGEMM 20.514 TFLOPS, 232.7 GB/s mem bandwidth; slave GPU 63.4% peak load (c2cd telemetry) |
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.5 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |
| T08 remote GPU as a local CUDA device | **PASS** | master sees 2 GPUs (remote: Xavier 30.3 GiB); remote results verified=True; remote/native: SGEMM 1.376/1.385 TFLOPS, HGEMM 9.864/10.014, membw 111.26/112.95 GB/s, alloc 19.0 GiB; link 469.1/459.6 MB/s, 267835 launches/s, sync 354.3 us |
| T09 one process uses both GPUs | **PASS** | one process, 2 GPUs: split SAXPY over 268435456 elements on 2 GPUs ok=True (3.822 s); per-GPU checks [True, True]; samples {'simpleMultiGPU': 'PASS', 'MonteCarloMultiGPU': 'PASS', 'deviceQuery': 'PASS'}; deviceQuery: Detected 2 CUDA Capable device(s) |
| T10 NVIDIA CUDA samples on the remote GPU | **PASS** | 49 unmodified samples: remote 48 pass / 1 waived / 0 fail; local 49 pass / 0 waived / 0 fail; remote-only failures: none; waived remotely: ['UnifiedMemoryStreams'] |
| T11 remote GPU survives link outage | **PASS** | remote GPU job ran through a 1.5 s link cut: exit 0, results verified=True, total 10.7 s, link UP |

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

### T01 link health (PASS, 2026-11-02 20:28:59)

```json
{
  "rtt_avg_ms": 0.309,
  "rtt_max_ms": 0.758,
  "rtt_mdev_ms": 0.029,
  "tcp_m2s_gbps": 3.711,
  "tcp_s2m_gbps": 3.718
}
```

### T02 cluster inventory (CPUs, RAM, GPUs) (PASS, 2026-11-02 20:29:00)

```json
{
  "master": {
    "host": "xavier",
    "cpus": 8,
    "cpu": "NVIDIA Carmel (ARMv8.2)",
    "cpu_max_mhz": 2265,
    "ram_gib": 14.54,
    "ram_avail_gib": 11.55,
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
    "ram_avail_gib": 24.32,
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
    "ram_avail_gib": 35.87,
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

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-02 20:30:38)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 44.96,
  "pooled_s": 26.93,
  "speedup": 1.67,
  "master_only_mib_s": 11.4,
  "pooled_mib_s": 19.0,
  "chunks_done_by_slave": 32,
  "compressed_mib": 140.8,
  "link_tx_mib": 259.5,
  "link_rx_mib": 74.6,
  "cpu_util_mid_run": {
    "master": 99.3,
    "slave": 99.7
  },
  "verified": true
}
```

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 20:32:30)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 48.03,
  "verify_s": 57.78,
  "write_gib_s": 0.167,
  "verify_gib_s": 0.138,
  "errors": 0,
  "rss_after_write_kb": 2081136,
  "swap_after_write_kb": 6308472,
  "peak_rss_kb": 2089664,
  "peak_swap_kb": 6308472,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.51
}
```

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-02 20:36:11)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 67.89,
  "verify_s": 140.89,
  "write_gib_s": 0.302,
  "verify_gib_s": 0.145,
  "errors": 0,
  "rss_after_write_kb": 8833856,
  "swap_after_write_kb": 12663140,
  "peak_rss_kb": 12583912,
  "peak_swap_kb": 12663140,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.33,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 13.0,
  "footprint_over_physical_gib": 5.96
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-02 20:36:22)

```json
{
  "master": {
    "host": "xavier",
    "name": "Xavier",
    "max_alloc_gib": 10.25,
    "gpu_total_mem_gib": 14.54,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.396,
    "hgemm_tc_tflops": 10.36,
    "membw_gbs": 117.99
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 19.25,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.391,
    "sgemm_tflops": 1.382,
    "hgemm_tc_tflops": 10.154,
    "membw_gbs": 114.71
  },
  "cluster": {
    "fp32_fma_tflops": 2.787,
    "sgemm_tflops": 2.778,
    "hgemm_tc_tflops": 20.514,
    "membw_gbs": 232.7,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 29.5
  },
  "gpu_util_peak": {
    "master": 44.4,
    "slave": 63.4
  }
}
```

### T07 remote memory survives link outage (PASS, 2026-11-02 20:36:56)

```json
{
  "data_mib": 1024,
  "write_mib_s": 340.5,
  "link_outage_s": 1.55,
  "read_total_s": 5.98,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

### T08 remote GPU as a local CUDA device (PASS, 2026-11-02 20:37:28)

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
    "htod_pinned_mbs": 469.1,
    "dtoh_pinned_mbs": 459.6,
    "htod_pageable_mbs": 468.5,
    "dtoh_pageable_mbs": 423.3,
    "async_launches_per_s": 267835,
    "launch_sync_us": 354.3,
    "dtoh_4b_us": 403.9
  },
  "native_on_slave": {
    "fp32_fma_tflops": 1.39,
    "sgemm_tflops": 1.385,
    "hgemm_tc_tflops": 10.014,
    "membw_gbs": 112.95,
    "max_alloc": 20401094656
  },
  "remote_from_master": {
    "fp32_fma_tflops": 1.386,
    "sgemm_tflops": 1.376,
    "hgemm_tc_tflops": 9.864,
    "membw_gbs": 111.26,
    "max_alloc": 20401094656
  },
  "remote_vs_native": {
    "fp32_fma_tflops": 0.997,
    "sgemm_tflops": 0.994,
    "hgemm_tc_tflops": 0.985,
    "membw_gbs": 0.985
  }
}
```

### T09 one process uses both GPUs (PASS, 2026-11-02 20:37:53)

```json
{
  "gpus": 2,
  "split_saxpy": {
    "devices": 2,
    "elements": 268435456,
    "seconds": 3.822,
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
      "htod_pinned_mbs": 37427.7,
      "async_launches_per_s": 170810
    },
    {
      "device": 1,
      "pci_domain": 3116,
      "ok": true,
      "saxpy_ok": true,
      "sgemm_ok": true,
      "streams_ok": true,
      "htod_pinned_mbs": 469.4,
      "async_launches_per_s": 264739
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

### T10 NVIDIA CUDA samples on the remote GPU (PASS, 2026-11-02 20:40:39)

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
      "remote_s": 1.78,
      "local_s": 1.65
    },
    "MersenneTwisterGP11213": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.45,
      "local_s": 0.3
    },
    "MonteCarloMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.42,
      "local_s": 0.29
    },
    "SobolQRNG": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.56,
      "local_s": 0.35
    },
    "UnifiedMemoryStreams": {
      "local": "PASS",
      "remote": "WAIVED",
      "remote_s": 0.33,
      "local_s": 2.87
    },
    "asyncAPI": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.85,
      "local_s": 0.43
    },
    "bandwidthTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 14.67,
      "local_s": 0.79
    },
    "batchCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.1,
      "local_s": 2.28
    },
    "binomialOptions": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 4.23,
      "local_s": 4.27
    },
    "clock": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.19
    },
    "concurrentKernels": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.18
    },
    "conjugateGradient": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.4,
      "local_s": 1.21
    },
    "convolutionSeparable": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.6,
      "local_s": 3.07
    },
    "cppIntegration": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.19
    },
    "dct8x8": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.78,
      "local_s": 0.64
    },
    "deviceQuery": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.06
    },
    "deviceQueryDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.18,
      "local_s": 0.06
    },
    "dwtHaar1D": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.19
    },
    "fastWalshTransform": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.51,
      "local_s": 2.26
    },
    "histogram": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.59,
      "local_s": 1.29
    },
    "inlinePTX": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "matrixMul": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.44,
      "local_s": 0.31
    },
    "matrixMulDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "mergeSort": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.66,
      "local_s": 0.42
    },
    "p2pBandwidthLatencyTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.52,
      "local_s": 0.35
    },
    "quasirandomGenerator": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.14,
      "local_s": 0.97
    },
    "reduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.91,
      "local_s": 0.55
    },
    "scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.97,
      "local_s": 1.79
    },
    "shfl_scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.38,
      "local_s": 0.23
    },
    "simpleAWBarrier": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.87,
      "local_s": 0.5
    },
    "simpleAtomicIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "simpleCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.98,
      "local_s": 2.3
    },
    "simpleCUBLASXT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 13.78,
      "local_s": 12.74
    },
    "simpleCUFFT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.35,
      "local_s": 0.44
    },
    "simpleCallback": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.19
    },
    "simpleCooperativeGroups": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "simpleMultiCopy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.17,
      "local_s": 0.76
    },
    "simpleMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.72,
      "local_s": 1.31
    },
    "simpleOccupancy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.37,
      "local_s": 0.23
    },
    "simpleStreams": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 29.97,
      "local_s": 1.17
    },
    "simpleTemplates": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "simpleTexture": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.36,
      "local_s": 0.23
    },
    "simpleVoteIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "simpleZeroCopy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.28
    },
    "sortingNetworks": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.51,
      "local_s": 3.18
    },
    "threadFenceReduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.46,
      "local_s": 0.25
    },
    "transpose": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.65,
      "local_s": 0.38
    },
    "vectorAdd": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "vectorAddDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.2
    }
  }
}
```

### T11 remote GPU survives link outage (PASS, 2026-11-02 20:40:51)

```json
{
  "exit_code": 0,
  "link_outage_s": 1.52,
  "run_s": 10.71,
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

