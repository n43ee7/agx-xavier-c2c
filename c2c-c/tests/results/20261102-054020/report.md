# C2C proof report (2026-11-02 05:52)

**10/10 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.322 ms avg, TCP 3.711 Gbit/s master->slave, 3.717 Gbit/s slave->master |
| T02 cluster inventory (CPUs, RAM, GPUs) | **PASS** | 16 CPUs (8+8), 44.8 GiB RAM (14.54+30.26), 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores, peak 2.82 FP32 TFLOPS |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 45.18 s, pooled 16 cores 26.51 s = 1.7x; slave did 30/64 chunks; CPU mid-run master 99.8% slave 99.1% |
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.25 GiB), write 0.177 GiB/s, verify 0.147 GiB/s, 0 errors |
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.07 GiB held in slave RAM, zram 0.0 GiB, write 0.284 GiB/s, verify 0.148 GiB/s, 0 errors |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 30.75 GiB allocated (10.5+20.25), FP32 2.792 TFLOPS, SGEMM 2.787 TFLOPS, tensor-core HGEMM 20.591 TFLOPS, 231.12 GB/s mem bandwidth; slave GPU 99.7% peak load (c2cd telemetry) |
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.5 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |
| T08 remote GPU as a local CUDA device | **PASS** | master sees 2 GPUs (remote: Xavier 30.3 GiB); remote results verified=True; remote/native: SGEMM 1.384/1.386 TFLOPS, HGEMM 10.097/10.104, membw 113.28/112.92 GB/s, alloc 19.8 GiB; link 469.2/459.1 MB/s, 260508 launches/s, sync 352.0 us |
| T09 one process uses both GPUs | **PASS** | one process, 2 GPUs: split SAXPY over 268435456 elements on 2 GPUs ok=True (3.832 s); per-GPU checks [True, True]; samples {'simpleMultiGPU': 'PASS', 'MonteCarloMultiGPU': 'PASS', 'deviceQuery': 'PASS'}; deviceQuery: Detected 2 CUDA Capable device(s) |
| T10 NVIDIA CUDA samples on the remote GPU | **PASS** | 49 unmodified samples: remote 48 pass / 1 waived / 0 fail; local 49 pass / 0 waived / 0 fail; remote-only failures: none; waived remotely: ['UnifiedMemoryStreams'] |

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

### T01 link health (PASS, 2026-11-02 05:40:33)

```json
{
  "rtt_avg_ms": 0.322,
  "rtt_max_ms": 0.603,
  "rtt_mdev_ms": 0.022,
  "tcp_m2s_gbps": 3.711,
  "tcp_s2m_gbps": 3.717
}
```

### T02 cluster inventory (CPUs, RAM, GPUs) (PASS, 2026-11-02 05:40:34)

```json
{
  "master": {
    "host": "xavier",
    "cpus": 8,
    "cpu": "NVIDIA Carmel (ARMv8.2)",
    "cpu_max_mhz": 2265,
    "ram_gib": 14.54,
    "ram_avail_gib": 11.35,
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
    "ram_avail_gib": 25.68,
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
    "ram_avail_gib": 37.03,
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

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-02 05:42:11)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 45.18,
  "pooled_s": 26.51,
  "speedup": 1.7,
  "master_only_mib_s": 11.3,
  "pooled_mib_s": 19.3,
  "chunks_done_by_slave": 30,
  "compressed_mib": 140.8,
  "link_tx_mib": 243.3,
  "link_rx_mib": 65.3,
  "cpu_util_mid_run": {
    "master": 99.8,
    "slave": 99.1
  },
  "verified": true
}
```

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 05:43:57)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 45.27,
  "verify_s": 54.6,
  "write_gib_s": 0.177,
  "verify_gib_s": 0.147,
  "errors": 0,
  "rss_after_write_kb": 2081164,
  "swap_after_write_kb": 6308464,
  "peak_rss_kb": 2089768,
  "peak_swap_kb": 6308560,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.25
}
```

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-02 05:47:38)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 72.09,
  "verify_s": 138.4,
  "write_gib_s": 0.284,
  "verify_gib_s": 0.148,
  "errors": 0,
  "rss_after_write_kb": 9824708,
  "swap_after_write_kb": 11672332,
  "peak_rss_kb": 13389416,
  "peak_swap_kb": 11672332,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.07,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 12.98,
  "footprint_over_physical_gib": 5.96
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-02 05:47:49)

```json
{
  "master": {
    "host": "xavier",
    "name": "Xavier",
    "max_alloc_gib": 10.5,
    "gpu_total_mem_gib": 14.54,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.396,
    "hgemm_tc_tflops": 10.358,
    "membw_gbs": 117.77
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 20.25,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.391,
    "hgemm_tc_tflops": 10.233,
    "membw_gbs": 113.35
  },
  "cluster": {
    "fp32_fma_tflops": 2.792,
    "sgemm_tflops": 2.787,
    "hgemm_tc_tflops": 20.591,
    "membw_gbs": 231.12,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 30.75
  },
  "gpu_util_peak": {
    "master": 48.5,
    "slave": 99.7
  }
}
```

### T07 remote memory survives link outage (PASS, 2026-11-02 05:48:23)

```json
{
  "data_mib": 1024,
  "write_mib_s": 345.0,
  "link_outage_s": 1.55,
  "read_total_s": 5.71,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

### T08 remote GPU as a local CUDA device (PASS, 2026-11-02 05:48:55)

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
      "total_mem": 32496132096,
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
    "htod_pinned_mbs": 469.2,
    "dtoh_pinned_mbs": 459.1,
    "htod_pageable_mbs": 469.5,
    "dtoh_pageable_mbs": 425.1,
    "async_launches_per_s": 260508,
    "launch_sync_us": 352.0,
    "dtoh_4b_us": 417.2
  },
  "native_on_slave": {
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.386,
    "hgemm_tc_tflops": 10.104,
    "membw_gbs": 112.92,
    "max_alloc": 21206401024
  },
  "remote_from_master": {
    "fp32_fma_tflops": 1.392,
    "sgemm_tflops": 1.384,
    "hgemm_tc_tflops": 10.097,
    "membw_gbs": 113.28,
    "max_alloc": 21206401024
  },
  "remote_vs_native": {
    "fp32_fma_tflops": 0.997,
    "sgemm_tflops": 0.999,
    "hgemm_tc_tflops": 0.999,
    "membw_gbs": 1.003
  }
}
```

### T09 one process uses both GPUs (PASS, 2026-11-02 05:49:19)

```json
{
  "gpus": 2,
  "split_saxpy": {
    "devices": 2,
    "elements": 268435456,
    "seconds": 3.832,
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
      "htod_pinned_mbs": 37224.8,
      "async_launches_per_s": 165798
    },
    {
      "device": 1,
      "pci_domain": 3116,
      "ok": true,
      "saxpy_ok": true,
      "sgemm_ok": true,
      "streams_ok": true,
      "htod_pinned_mbs": 467.4,
      "async_launches_per_s": 269443
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

### T10 NVIDIA CUDA samples on the remote GPU (PASS, 2026-11-02 05:52:04)

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
      "remote_s": 1.87,
      "local_s": 1.63
    },
    "MersenneTwisterGP11213": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.47,
      "local_s": 0.31
    },
    "MonteCarloMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.41,
      "local_s": 0.29
    },
    "SobolQRNG": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.55,
      "local_s": 0.37
    },
    "UnifiedMemoryStreams": {
      "local": "PASS",
      "remote": "WAIVED",
      "remote_s": 0.33,
      "local_s": 2.88
    },
    "asyncAPI": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.84,
      "local_s": 0.42
    },
    "bandwidthTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 14.68,
      "local_s": 0.79
    },
    "batchCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.05,
      "local_s": 2.25
    },
    "binomialOptions": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 4.27,
      "local_s": 4.25
    },
    "clock": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "concurrentKernels": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.17
    },
    "conjugateGradient": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.45,
      "local_s": 1.23
    },
    "convolutionSeparable": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.56,
      "local_s": 2.51
    },
    "cppIntegration": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.18
    },
    "dct8x8": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.74,
      "local_s": 0.6
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
      "local_s": 0.2
    },
    "fastWalshTransform": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.49,
      "local_s": 2.25
    },
    "histogram": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.57,
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
      "remote_s": 0.42,
      "local_s": 0.3
    },
    "matrixMulDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.2
    },
    "mergeSort": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.65,
      "local_s": 0.42
    },
    "p2pBandwidthLatencyTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.54,
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
      "local_s": 0.56
    },
    "scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.98,
      "local_s": 1.76
    },
    "shfl_scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.38,
      "local_s": 0.22
    },
    "simpleAWBarrier": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.86,
      "local_s": 0.5
    },
    "simpleAtomicIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.3,
      "local_s": 0.19
    },
    "simpleCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.98,
      "local_s": 2.26
    },
    "simpleCUBLASXT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 13.49,
      "local_s": 12.45
    },
    "simpleCUFFT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.4,
      "local_s": 0.43
    },
    "simpleCallback": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.2
    },
    "simpleCooperativeGroups": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.18
    },
    "simpleMultiCopy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.16,
      "local_s": 0.76
    },
    "simpleMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.78,
      "local_s": 1.29
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
      "remote_s": 29.99,
      "local_s": 1.16
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
      "local_s": 0.25
    },
    "simpleVoteIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
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
      "remote_s": 3.56,
      "local_s": 3.18
    },
    "threadFenceReduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.44,
      "local_s": 0.23
    },
    "transpose": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.64,
      "local_s": 0.36
    },
    "vectorAdd": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.18
    },
    "vectorAddDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.19
    }
  }
}
```

