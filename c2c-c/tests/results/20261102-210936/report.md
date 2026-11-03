# C2C proof report (2026-11-02 21:09)

**1/1 tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.

| test | result | summary |
|---|---|---|
| T01 link health | **PASS** | RTT 0.334 ms avg, TCP 3.712 Gbit/s master->slave, 3.717 Gbit/s slave->master |

## Raw metrics

### T01 link health (PASS, 2026-11-02 21:09:49)

```json
{
  "rtt_avg_ms": 0.334,
  "rtt_max_ms": 0.949,
  "rtt_mdev_ms": 0.058,
  "tcp_m2s_gbps": 3.712,
  "tcp_s2m_gbps": 3.717
}
```

