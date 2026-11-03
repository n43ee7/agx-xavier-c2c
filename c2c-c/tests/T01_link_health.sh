#!/bin/bash
# T01 link health. Everything else sits on this link, so check it first:
# handshake state, RTT (ping over c2c0) and TCP throughput each way (iperf3).
# PASS: link UP, avg RTT < 1 ms, >= 3 Gbit/s both directions.
set -uo pipefail
TEST_ID=T01; TEST_NAME="link health"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link

PEER_IP="$(link_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["peer_ip"])')"
say "ping x500 over c2c0"
# "rtt min/avg/max/mdev = 0.198/0.301/0.412/0.040 ms" -> "avg max mdev"
rtt="$(ping -q -c 500 -i 0.002 "$PEER_IP" | sed -n 's|.*= \([0-9.]*\)/\([0-9.]*\)/\([0-9.]*\)/\([0-9.]*\).*|\2 \3 \4|p')"
read -r rtt_avg rtt_max rtt_mdev <<<"$rtt"

iperf_run() {   # $1 = extra args, prints Gbit/s
    c2crun "iperf3 -s -1 -B $PEER_IP >/dev/null 2>&1 & sleep 0.5"
    iperf3 -c "$PEER_IP" -t 5 -J $1 2>/dev/null |
        python3 -c 'import json,sys; print("%.3f" % (json.load(sys.stdin)["end"]["sum_received"]["bits_per_second"]/1e9))'
}
say "iperf3 master -> slave (5 s)"; m2s="$(iperf_run "")"
say "iperf3 slave -> master (5 s)"; s2m="$(iperf_run "-R")"

metrics="$(python3 - "$rtt_avg" "$rtt_max" "$rtt_mdev" "$m2s" "$s2m" <<'EOF'
import json, sys
a = [float(x) if x else 0 for x in sys.argv[1:]]
print(json.dumps({"rtt_avg_ms": a[0], "rtt_max_ms": a[1], "rtt_mdev_ms": a[2],
                  "tcp_m2s_gbps": a[3], "tcp_s2m_gbps": a[4]}))
EOF
)"
summary="RTT ${rtt_avg} ms avg, TCP ${m2s} Gbit/s master->slave, ${s2m} Gbit/s slave->master"
if python3 -c "import sys; sys.exit(0 if 0 < $rtt_avg < 1 and $m2s >= 3 and $s2m >= 3 else 1)"; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary" "$metrics"
fi
