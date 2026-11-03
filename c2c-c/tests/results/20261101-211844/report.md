# C2C proof report (2026-11-01 21:28)

**7/7 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.300 ms avg, TCP 3.712 Gbit/s master->slave, 3.719 Gbit/s slave->master |
| T02 cluster inventory (CPUs, RAM, GPUs) | **PASS** | 16 CPUs (8+8), 44.8 GiB RAM (14.54+30.26), 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores, peak 2.82 FP32 TFLOPS |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 44.18 s, pooled 16 cores 26.2 s = 1.69x; slave did 30/64 chunks; CPU mid-run master 100.0% slave 99.8% |
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.25 GiB), write 0.173 GiB/s, verify 0.083 GiB/s, 0 errors |
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.8 GiB held in slave RAM, zram 0.0 GiB, write 0.391 GiB/s, verify 0.087 GiB/s, 0 errors |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 30.25 GiB allocated (10.5+19.75), FP32 2.792 TFLOPS, SGEMM 2.786 TFLOPS, tensor-core HGEMM 20.598 TFLOPS, 233.54 GB/s mem bandwidth; slave GPU 99.7% peak load (c2cd telemetry) |
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.5 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |

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

### T01 link health (PASS, 2026-11-01 21:18:58)

```json
{
  "rtt_avg_ms": 0.3,
  "rtt_max_ms": 0.649,
  "rtt_mdev_ms": 0.027,
  "tcp_m2s_gbps": 3.712,
  "tcp_s2m_gbps": 3.719
}
```

### T02 cluster inventory (CPUs, RAM, GPUs) (PASS, 2026-11-01 21:18:59)

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
    "ram_avail_gib": 25.11,
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
    "ram_avail_gib": 36.46,
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

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-01 21:20:35)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 44.18,
  "pooled_s": 26.2,
  "speedup": 1.69,
  "master_only_mib_s": 11.6,
  "pooled_mib_s": 19.5,
  "chunks_done_by_slave": 30,
  "compressed_mib": 140.8,
  "link_tx_mib": 243.6,
  "link_rx_mib": 72.9,
  "cpu_util_mid_run": {
    "master": 100.0,
    "slave": 99.8
  },
  "verified": true
}
```

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-01 21:23:05)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 46.23,
  "verify_s": 96.44,
  "write_gib_s": 0.173,
  "verify_gib_s": 0.083,
  "errors": 0,
  "rss_after_write_kb": 2081576,
  "swap_after_write_kb": 6308232,
  "peak_rss_kb": 2089768,
  "peak_swap_kb": 6308564,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.25
}
```

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-01 21:28:10)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 52.5,
  "verify_s": 235.05,
  "write_gib_s": 0.391,
  "verify_gib_s": 0.087,
  "errors": 0,
  "rss_after_write_kb": 8312288,
  "swap_after_write_kb": 13184876,
  "peak_rss_kb": 13040096,
  "peak_swap_kb": 13184876,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.8,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 13.1,
  "footprint_over_physical_gib": 5.96
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-01 21:28:21)

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
    "membw_gbs": 117.97
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 19.75,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.39,
    "hgemm_tc_tflops": 10.24,
    "membw_gbs": 115.57
  },
  "cluster": {
    "fp32_fma_tflops": 2.792,
    "sgemm_tflops": 2.786,
    "hgemm_tc_tflops": 20.598,
    "membw_gbs": 233.54,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 30.25
  },
  "gpu_util_peak": {
    "master": 0.8,
    "slave": 99.7
  }
}
```

### T07 remote memory survives link outage (PASS, 2026-11-01 21:28:55)

```json
{
  "data_mib": 1024,
  "write_mib_s": 339.9,
  "link_outage_s": 1.52,
  "read_total_s": 5.98,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

