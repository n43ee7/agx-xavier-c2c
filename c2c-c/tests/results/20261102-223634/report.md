# C2C proof report (2026-11-02 22:37)

**3/3 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T08 remote GPU as a local CUDA device | **PASS** | master sees 2 GPUs (remote: Xavier 30.3 GiB); remote results verified=True; remote/native: SGEMM 1.374/1.383 TFLOPS, HGEMM 9.745/10.017, membw 108.92/111.1 GB/s, alloc 19.0 GiB; link 471.3/462.6 MB/s, 245930 launches/s, sync 163.4 us, via /dev/c2cch0 |
| T12 raw USB channels (TCP-free GPU data path) | **PASS** | raw: up 470.8 / down 459.7 / both 871.2 MB/s, sync p50 137.9 us (TCP 343.0 us), 4B read p50 165.0 us (TCP 367.3), GPU0<->GPU1 468.6/465.8 MB/s; 5 programs at once: 4 raw + 1 TCP, all ok=True |
| T13 raw channel: clean failure and recovery after a USB cut | **PASS** | USB cut 1.5 s: running program ended in 0.0 s with exit 2 (no hang=True); link UP after 2.5 s; new raw session /dev/c2cch0: up 470.3 / down 443.5 MB/s, sync p50 139.3 us |

## Raw metrics

### T08 remote GPU as a local CUDA device (PASS, 2026-11-02 22:37:05)

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
    "dtoh_pinned_mbs": 462.6,
    "htod_pageable_mbs": 473.1,
    "dtoh_pageable_mbs": 426.8,
    "async_launches_per_s": 245930,
    "launch_sync_us": 163.4,
    "dtoh_4b_us": 173.4
  },
  "native_on_slave": {
    "fp32_fma_tflops": 1.388,
    "sgemm_tflops": 1.383,
    "hgemm_tc_tflops": 10.017,
    "membw_gbs": 111.1,
    "max_alloc": 20937965568
  },
  "remote_from_master": {
    "fp32_fma_tflops": 1.391,
    "sgemm_tflops": 1.374,
    "hgemm_tc_tflops": 9.745,
    "membw_gbs": 108.92,
    "max_alloc": 20401094656
  },
  "remote_vs_native": {
    "fp32_fma_tflops": 1.002,
    "sgemm_tflops": 0.993,
    "hgemm_tc_tflops": 0.973,
    "membw_gbs": 0.98
  },
  "transport": "/dev/c2cch0"
}
```

### T12 raw USB channels (TCP-free GPU data path) (PASS, 2026-11-02 22:37:27)

```json
{
  "raw": {
    "h2d_mbs": 470.8,
    "d2h_mbs": 459.7,
    "bidir_mbs": 871.2,
    "sync_p50_us": 137.9,
    "sync_p99_us": 170.6,
    "read4_p50_us": 165.0,
    "peer_to_mbs": 468.6,
    "peer_from_mbs": 465.8
  },
  "tcp": {
    "h2d_mbs": 462.6,
    "d2h_mbs": 459.3,
    "bidir_mbs": 854.3,
    "sync_p50_us": 343.0,
    "sync_p99_us": 418.3,
    "read4_p50_us": 367.3,
    "peer_to_mbs": 457.6,
    "peer_from_mbs": 466.1
  },
  "channels": 4,
  "concurrent": {
    "programs": 5,
    "raw": 4,
    "tcp": 1,
    "all_ok": true,
    "h2d_mbs": [
      93.5,
      93.2,
      94.2,
      84.1,
      93.6
    ]
  }
}
```

### T13 raw channel: clean failure and recovery after a USB cut (PASS, 2026-11-02 22:37:40)

```json
{
  "first_program_exit": 2,
  "ended_after_cut_s": 0.0,
  "no_hang": true,
  "link_state": "UP",
  "link_back_s": 2.5,
  "after": {
    "h2d_mbs": 470.3,
    "d2h_mbs": 443.5,
    "bidir_mbs": 850.5,
    "sync_p50_us": 139.3
  },
  "after_via": "/dev/c2cch0"
}
```

