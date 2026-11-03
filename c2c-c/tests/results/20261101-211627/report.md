# C2C proof report (2026-11-01 21:17)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T07 remote memory survives link outage | **PASS** | 1 GiB in slave RAM; link cut for 1.5 s mid-read; read resumed, data identical, device still attached; kfix loaded=1 |

## Raw metrics

### T07 remote memory survives link outage (PASS, 2026-11-01 21:17:01)

```json
{
  "data_mib": 1024,
  "write_mib_s": 345.9,
  "link_outage_s": 1.51,
  "read_total_s": 5.73,
  "link_state_after": "UP",
  "dd_exit": 0,
  "still_attached": true,
  "data_identical": true,
  "c2c_kfix_loaded": true
}
```

