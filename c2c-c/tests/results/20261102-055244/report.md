# C2C proof report (2026-11-02 05:56)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T05 RAM pool: one program larger than master RAM | **PASS** | one process used 20.5 GiB on a 14.5 GiB master (+5.96 GiB over physical): 12.08 GiB held in slave RAM, zram 0.0 GiB, write 0.321 GiB/s, verify 0.149 GiB/s, 0 errors |

## Raw metrics

### T05 RAM pool: one program larger than master RAM (PASS, 2026-11-02 05:56:17)

```json
{
  "gib": 20.5,
  "bytes": 22011707392,
  "write_s": 63.84,
  "verify_s": 137.71,
  "write_gib_s": 0.321,
  "verify_gib_s": 0.149,
  "errors": 0,
  "rss_after_write_kb": 9123180,
  "swap_after_write_kb": 12373688,
  "peak_rss_kb": 12390356,
  "peak_swap_kb": 12373688,
  "master_physical_gib": 14.54,
  "exit_code": 0,
  "peak_remote_swap_gib": 12.08,
  "peak_zram_gib": 0.0,
  "peak_slave_memd_resident_gib": 12.93,
  "footprint_over_physical_gib": 5.96
}
```

