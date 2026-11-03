# C2C proof report (2026-11-02 04:45)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **PASS** | 8 GiB program capped at 2G RAM: 8.0 GiB paged into slave RAM (memd resident 8.25 GiB), write 0.177 GiB/s, verify 0.117 GiB/s, 0 errors |

## Raw metrics

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (PASS, 2026-11-02 04:45:44)

```json
{
  "gib": 8.0,
  "bytes": 8589934592,
  "write_s": 45.16,
  "verify_s": 68.46,
  "write_gib_s": 0.177,
  "verify_gib_s": 0.117,
  "errors": 0,
  "rss_after_write_kb": 2081192,
  "swap_after_write_kb": 6308520,
  "peak_rss_kb": 2089312,
  "peak_swap_kb": 6308576,
  "ram_cap": "2G",
  "exit_code": 0,
  "peak_remote_swap_gib": 8.0,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 8.25
}
```

