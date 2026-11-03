# C2C proof report (2026-11-01 20:03)

**0/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) | **FAIL** | 8 GiB program capped at 2G RAM:  GiB paged into slave RAM (memd resident  GiB), write  GiB/s, verify  GiB/s,  errors (exit 0) |

## Raw metrics

### T04 RAM pool: remote paging (2 GiB cap, 8 GiB program) (FAIL, 2026-11-01 20:03:42)

```json
{}
```

