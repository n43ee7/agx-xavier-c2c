# C2C proof report (2026-11-01 20:16)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 2.0 GiB), write 0.169 GiB/s, verify 0.046 GiB/s, 0 errors |

## Raw metrics

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-01 20:16:58)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 47.42,
  "verify_s": 172.69,
  "write_gib_s": 0.169,
  "verify_gib_s": 0.046,
  "errors": 0,
  "rss_after_write_kb": 2081696,
  "swap_after_write_kb": 6308276,
  "peak_rss_kb": 2089668,
  "peak_swap_kb": 6308276,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 2.0
}
```

