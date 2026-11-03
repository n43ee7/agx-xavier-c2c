# C2C proof report (2026-11-02 05:52)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T11 remote GPU survives link outage | **PASS** | remote GPU job ran through a 1.5 s link cut: exit 0, results verified=True, total 10.8 s, link UP |

## Raw metrics

### T11 remote GPU survives link outage (PASS, 2026-11-02 05:52:44)

```json
{
  "exit_code": 0,
  "link_outage_s": 1.5,
  "run_s": 10.8,
  "link_state_after": "UP",
  "checks": {
    "saxpy_ok": true,
    "reduction_ok": true,
    "sgemm_ok": true,
    "struct_param_ok": true,
    "streams_ok": true
  },
  "ran_on_remote": true
}
```

