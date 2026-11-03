# C2C proof report (2026-11-02 22:04)

**0/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **FAIL** | RTT 0.312 ms avg, TCP 0.330 Gbit/s master->slave, 2.026 Gbit/s slave->master |

## Raw metrics

### T01 link health (FAIL, 2026-11-02 22:04:13)

```json
{
  "rtt_avg_ms": 0.312,
  "rtt_max_ms": 0.902,
  "rtt_mdev_ms": 0.042,
  "tcp_m2s_gbps": 0.33,
  "tcp_s2m_gbps": 2.026
}
```

