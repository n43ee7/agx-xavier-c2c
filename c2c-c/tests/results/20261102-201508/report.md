# C2C proof report (2026-11-02 20:27)

**11/11 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.538 ms avg, TCP 3.712 Gbit/s master->slave, 3.714 Gbit/s slave->master |
| T02 cluster inventory (CPUs, RAM, GPUs) | **PASS** | 16 CPUs (8+8), 44.8 GiB RAM (14.54+30.26), 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores, peak 2.82 FP32 TFLOPS |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 44.45 s, pooled 16 cores 26.97 s = 1.65x; slave did 29/64 chunks; CPU mid-run master 99.9% slave 85.1% |
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.51 GiB), write 0.162 GiB/s, verify 0.135 GiB/s, 0 errors |
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.08 GiB held in slave RAM, zram 0.0 GiB, write 0.332 GiB/s, verify 0.139 GiB/s, 0 errors |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 32.5 GiB allocated (10.25+22.25), FP32 2.789 TFLOPS, SGEMM 2.03 TFLOPS, tensor-core HGEMM 15.067 TFLOPS, 176.39 GB/s mem bandwidth; slave GPU 95.6% peak load (c2cd telemetry) |
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.6 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |
| T08 remote GPU as a local CUDA device | **PASS** | master sees 2 GPUs (remote: Xavier 30.3 GiB); remote results verified=True; remote/native: SGEMM 1.379/1.391 TFLOPS, HGEMM 9.908/10.168, membw 111.0/114.62 GB/s, alloc 19.0 GiB; link 467.8/458.9 MB/s, 127338 launches/s, sync 391.6 us |
| T09 one process uses both GPUs | **PASS** | one process, 2 GPUs: split SAXPY over 268435456 elements on 2 GPUs ok=True (3.833 s); per-GPU checks [True, True]; samples {'simpleMultiGPU': 'PASS', 'MonteCarloMultiGPU': 'PASS', 'deviceQuery': 'PASS'}; deviceQuery: Detected 2 CUDA Capable device(s) |
| T10 NVIDIA CUDA samples on the remote GPU | **PASS** | 49 unmodified samples: remote 48 pass / 1 waived / 0 fail; local 49 pass / 0 waived / 0 fail; remote-only failures: none; waived remotely: ['UnifiedMemoryStreams'] |
| T11 remote GPU survives link outage | **PASS** | remote GPU job ran through a 1.5 s link cut: exit 0, results verified=True, total 11.0 s, link UP |

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

### T01 link health (PASS, 2026-11-02 20:15:21)

```json
{
  "rtt_avg_ms": 0.538,
  "rtt_max_ms": 1.035,
  "rtt_mdev_ms": 0.048,
  "tcp_m2s_gbps": 3.712,
  "tcp_s2m_gbps": 3.714
}
```

### T02 cluster inventory (CPUs, RAM, GPUs) (PASS, 2026-11-02 20:15:22)

```json
{
  "master": {
    "host": "xavier",
    "cpus": 8,
    "cpu": "NVIDIA Carmel (ARMv8.2)",
    "cpu_max_mhz": 2265,
    "ram_gib": 14.54,
    "ram_avail_gib": 12.71,
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
    "ram_avail_gib": 27.99,
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
    "ram_avail_gib": 40.7,
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

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-02 20:17:00)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 44.45,
  "pooled_s": 26.97,
  "speedup": 1.65,
  "master_only_mib_s": 11.5,
  "pooled_mib_s": 19.0,
  "chunks_done_by_slave": 29,
  "compressed_mib": 140.8,
  "link_tx_mib": 236.0,
  "link_rx_mib": 65.8,
  "cpu_util_mid_run": {
    "master": 99.9,
    "slave": 85.1
  },
  "verified": true
}
```

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 20:18:56)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 49.51,
  "verify_s": 59.06,
  "write_gib_s": 0.162,
  "verify_gib_s": 0.135,
  "errors": 0,
  "rss_after_write_kb": 2081476,
  "swap_after_write_kb": 6308396,
  "peak_rss_kb": 2089600,
  "peak_swap_kb": 6308396,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.51
}
```

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-02 20:22:37)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 61.67,
  "verify_s": 147.39,
  "write_gib_s": 0.332,
  "verify_gib_s": 0.139,
  "errors": 0,
  "rss_after_write_kb": 11351136,
  "swap_after_write_kb": 10145728,
  "peak_rss_kb": 11533360,
  "peak_swap_kb": 10145728,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.08,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 12.63,
  "footprint_over_physical_gib": 5.96
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-02 20:22:58)

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
    "membw_gbs": 117.96
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 22.25,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.393,
    "sgemm_tflops": 0.634,
    "hgemm_tc_tflops": 4.707,
    "membw_gbs": 58.43
  },
  "cluster": {
    "fp32_fma_tflops": 2.789,
    "sgemm_tflops": 2.03,
    "hgemm_tc_tflops": 15.067,
    "membw_gbs": 176.39,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 32.5
  },
  "gpu_util_peak": {
    "master": 37.9,
    "slave": 95.6
  }
}
```

### T07 remote memory survives link outage (PASS, 2026-11-02 20:23:33)

```json
{
  "data_mib": 1024,
  "write_mib_s": 335.6,
  "link_outage_s": 1.56,
  "read_total_s": 5.89,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

### T08 remote GPU as a local CUDA device (PASS, 2026-11-02 20:24:06)

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
    "htod_pinned_mbs": 467.8,
    "dtoh_pinned_mbs": 458.9,
    "htod_pageable_mbs": 466.2,
    "dtoh_pageable_mbs": 424.4,
    "async_launches_per_s": 127338,
    "launch_sync_us": 391.6,
    "dtoh_4b_us": 434.4
  },
  "native_on_slave": {
    "fp32_fma_tflops": 1.392,
    "sgemm_tflops": 1.391,
    "hgemm_tc_tflops": 10.168,
    "membw_gbs": 114.62,
    "max_alloc": 20401094656
  },
  "remote_from_master": {
    "fp32_fma_tflops": 1.385,
    "sgemm_tflops": 1.379,
    "hgemm_tc_tflops": 9.908,
    "membw_gbs": 111.0,
    "max_alloc": 20401094656
  },
  "remote_vs_native": {
    "fp32_fma_tflops": 0.995,
    "sgemm_tflops": 0.991,
    "hgemm_tc_tflops": 0.974,
    "membw_gbs": 0.968
  }
}
```

### T09 one process uses both GPUs (PASS, 2026-11-02 20:24:31)

```json
{
  "gpus": 2,
  "split_saxpy": {
    "devices": 2,
    "elements": 268435456,
    "seconds": 3.833,
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
      "htod_pinned_mbs": 37287.6,
      "async_launches_per_s": 168863
    },
    {
      "device": 1,
      "pci_domain": 3116,
      "ok": true,
      "saxpy_ok": true,
      "sgemm_ok": true,
      "streams_ok": true,
      "htod_pinned_mbs": 467.5,
      "async_launches_per_s": 258295
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

### T10 NVIDIA CUDA samples on the remote GPU (PASS, 2026-11-02 20:27:24)

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
      "remote_s": 2.07,
      "local_s": 1.35
    },
    "MersenneTwisterGP11213": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.48,
      "local_s": 0.32
    },
    "MonteCarloMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.49,
      "local_s": 0.3
    },
    "SobolQRNG": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.5,
      "local_s": 0.33
    },
    "UnifiedMemoryStreams": {
      "local": "PASS",
      "remote": "WAIVED",
      "remote_s": 0.33,
      "local_s": 2.97
    },
    "asyncAPI": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.88,
      "local_s": 0.45
    },
    "bandwidthTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 15.08,
      "local_s": 0.83
    },
    "batchCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.21,
      "local_s": 2.4
    },
    "binomialOptions": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 4.4,
      "local_s": 4.25
    },
    "clock": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "concurrentKernels": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "conjugateGradient": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.52,
      "local_s": 1.25
    },
    "convolutionSeparable": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 2.65,
      "local_s": 3.11
    },
    "cppIntegration": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "dct8x8": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.82,
      "local_s": 0.65
    },
    "deviceQuery": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.07
    },
    "deviceQueryDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.18,
      "local_s": 0.07
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
      "remote_s": 2.52,
      "local_s": 2.26
    },
    "histogram": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.6,
      "local_s": 1.33
    },
    "inlinePTX": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.18
    },
    "matrixMul": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.48,
      "local_s": 0.31
    },
    "matrixMulDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.32,
      "local_s": 0.2
    },
    "mergeSort": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.66,
      "local_s": 0.43
    },
    "p2pBandwidthLatencyTest": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.69,
      "local_s": 0.35
    },
    "quasirandomGenerator": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.16,
      "local_s": 1.0
    },
    "reduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.44,
      "local_s": 0.57
    },
    "scan": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.87,
      "local_s": 1.8
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
      "remote_s": 0.96,
      "local_s": 0.51
    },
    "simpleAtomicIntrinsics": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.2
    },
    "simpleCUBLAS": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.16,
      "local_s": 2.38
    },
    "simpleCUBLASXT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 14.21,
      "local_s": 12.66
    },
    "simpleCUFFT": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.37,
      "local_s": 0.47
    },
    "simpleCallback": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.35,
      "local_s": 0.21
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
      "remote_s": 2.23,
      "local_s": 0.76
    },
    "simpleMultiGPU": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 1.75,
      "local_s": 1.38
    },
    "simpleOccupancy": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.39,
      "local_s": 0.24
    },
    "simpleStreams": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 31.62,
      "local_s": 1.18
    },
    "simpleTemplates": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.19
    },
    "simpleTexture": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.38,
      "local_s": 0.23
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
      "remote_s": 0.31,
      "local_s": 0.29
    },
    "sortingNetworks": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 3.63,
      "local_s": 3.2
    },
    "threadFenceReduction": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.48,
      "local_s": 0.25
    },
    "transpose": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.69,
      "local_s": 0.38
    },
    "vectorAdd": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.31,
      "local_s": 0.2
    },
    "vectorAddDrv": {
      "local": "PASS",
      "remote": "PASS",
      "remote_s": 0.33,
      "local_s": 0.21
    }
  }
}
```

### T11 remote GPU survives link outage (PASS, 2026-11-02 20:27:36)

```json
{
  "exit_code": 0,
  "link_outage_s": 1.53,
  "run_s": 11.05,
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

