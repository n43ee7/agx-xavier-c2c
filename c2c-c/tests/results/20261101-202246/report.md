# C2C proof report (2026-11-01 20:26)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 16.5 GiB on a 14.5 GiB master (+1.96 GiB over physical): 11.83 GiB held in slave RAM, zram 0.0 GiB, write 0.431 GiB/s, verify 0.115 GiB/s, 0 errors |

## Raw metrics

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-01 20:26:05)

```json
{
  "gib": 16.5,
  "bytes": 17716740096,
  "write_s": 38.29,
  "verify_s": 144.03,
  "write_gib_s": 0.431,
  "verify_gib_s": 0.115,
  "errors": 0,
  "rss_after_write_kb": 8418064,
  "swap_after_write_kb": 8884688,
  "peak_rss_kb": 13212864,
  "peak_swap_kb": 8884688,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 11.83,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 12.75,
  "footprint_over_physical_gib": 1.96
}
```

