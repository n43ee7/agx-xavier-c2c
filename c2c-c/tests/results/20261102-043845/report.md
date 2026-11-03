# C2C proof report (2026-11-02 04:41)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.51 GiB), write 0.175 GiB/s, verify 0.083 GiB/s, 0 errors |

## Raw metrics

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 04:41:14)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 45.75,
  "verify_s": 96.88,
  "write_gib_s": 0.175,
  "verify_gib_s": 0.083,
  "errors": 0,
  "rss_after_write_kb": 2081604,
  "swap_after_write_kb": 6308260,
  "peak_rss_kb": 2089592,
  "peak_swap_kb": 6308340,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.51
}
```

