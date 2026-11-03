# C2C proof report (2026-11-01 19:59)

**3/3 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.299 ms avg, TCP 3.716 Gbit/s master->slave, 3.718 Gbit/s slave->master |
| T03 CPU pool (16 cores, unmodified xz) | **PASS** | xz 512 MiB: master-only 43.38 s, pooled 16 cores 25.65 s = 1.69x; slave did 29/64 chunks; CPU mid-run master 100.0% slave 99.4% |
| T06 GPU pool (2x Volta, memory + compute) | **PASS** | 2 GPUs together: 33.0 GiB allocated (10.25+22.75), FP32 2.792 TFLOPS, SGEMM 2.786 TFLOPS, tensor-core HGEMM 20.566 TFLOPS, 232.58 GB/s mem bandwidth; slave GPU 0.0% busy mid-run |

## Raw metrics

### T01 link health (PASS, 2026-11-01 19:57:26)

```json
{
  "rtt_avg_ms": 0.299,
  "rtt_max_ms": 0.633,
  "rtt_mdev_ms": 0.025,
  "tcp_m2s_gbps": 3.716,
  "tcp_s2m_gbps": 3.718
}
```

### T03 CPU pool (16 cores, unmodified xz) (PASS, 2026-11-01 19:59:01)

```json
{
  "data_mib": 512,
  "chunks": 64,
  "chunk_mib": 8,
  "master_only_s": 43.38,
  "pooled_s": 25.65,
  "speedup": 1.69,
  "master_only_mib_s": 11.8,
  "pooled_mib_s": 20.0,
  "chunks_done_by_slave": 29,
  "compressed_mib": 140.8,
  "link_tx_mib": 235.2,
  "link_rx_mib": 61.4,
  "cpu_util_mid_run": {
    "master": 100.0,
    "slave": 99.4
  },
  "verified": true
}
```

### T06 GPU pool (2x Volta, memory + compute) (PASS, 2026-11-01 19:59:17)

```json
{
  "master": {
    "host": "xavier",
    "name": "Xavier",
    "max_alloc_gib": 10.25,
    "gpu_total_mem_gib": 14.54,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.396,
    "hgemm_tc_tflops": 10.369,
    "membw_gbs": 118.27
  },
  "slave": {
    "host": "xavier32",
    "name": "Xavier",
    "max_alloc_gib": 22.75,
    "gpu_total_mem_gib": 30.26,
    "fp32_fma_tflops": 1.396,
    "sgemm_tflops": 1.39,
    "hgemm_tc_tflops": 10.197,
    "membw_gbs": 114.31
  },
  "cluster": {
    "fp32_fma_tflops": 2.792,
    "sgemm_tflops": 2.786,
    "hgemm_tc_tflops": 20.566,
    "membw_gbs": 232.58,
    "gpus": 2,
    "sms": 16,
    "cuda_cores": 1024,
    "tensor_cores": 128,
    "max_alloc_gib": 33.0
  },
  "gpu_util_mid_run": {
    "master": 0.0,
    "slave": 0.0
  }
}
```

