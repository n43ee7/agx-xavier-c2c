#!/bin/bash
# T07 remote RAM vs a link outage. Write 1 GiB of random data into the slave's
# lent RAM (c2c-rmem, /dev/nbd0). While reading it back, restart the master's
# c2cd: link goes DOWN, c2c0 is destroyed + recreated, keyed handshake again.
# The read should just stall, resume and give back identical data, and the
# device has to stay attached. Also checks c2c_kfix is loaded and active.
#
# PASS: data identical, nbd still attached, link UP again, kfix loaded.
set -uo pipefail
TEST_ID=T07; TEST_NAME="remote memory survives link outage"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link

W=/var/tmp/c2c-t07; DEV=/dev/nbd0; MB=1024
rm -rf "$W"; mkdir -p "$W" "$RESULTS"
cleanup() {
    c2c-rmem status -d $DEV >/dev/null 2>&1 && c2c-rmem detach -d $DEV >/dev/null
    c2crun "pkill -x c2c-memd" 2>/dev/null; rm -rf "$W"
}
trap cleanup EXIT

kfix=0; [ -d /sys/module/c2c_kfix ] && kfix=1
say "kernel fix c2c_kfix loaded: $kfix"

c2crun "pkill -x c2c-memd; sleep 0.3; nohup /opt/c2c/bin/c2c-memd -s 2G >/tmp/c2c-memd.log 2>&1 </dev/null &"
sleep 1
c2c-rmem attach -d $DEV -c 2 >&2 || { result FAIL "attach failed"; exit 1; }

say "writing $MB MiB of random data into slave RAM"
head -c $((MB << 20)) /dev/urandom > "$W/ref"
ref=$(sha256sum < "$W/ref" | cut -d' ' -f1)
t0=$(now); dd if="$W/ref" of=$DEV bs=4M oflag=direct status=none; t1=$(now)

say "reading back while the link is cut (c2cd restart)"
( dd if=$DEV of="$W/back" bs=1M count=$MB iflag=direct status=none; echo $? > "$W/rc" ) &
rd=$!
sleep 0.5
tb=$(now); systemctl restart c2cd
# wait for the link
for i in $(seq 1 60); do
    st=$(link_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])' 2>/dev/null)
    [ "$st" = UP ] && break; sleep 0.5
done
tu=$(now)
wait $rd; t2=$(now)
back=$(sha256sum < "$W/back" | cut -d' ' -f1)
attached=0; c2c-rmem status -d $DEV >/dev/null 2>&1 && attached=1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)

metrics="$(python3 - <<EOF
import json
print(json.dumps({"data_mib": $MB, "write_mib_s": round($MB / ($t1 - $t0), 1),
  "link_outage_s": round($tu - $tb, 2), "read_total_s": round($t2 - $tb + 0.5, 2),
  "link_state_after": "$st", "dd_exit": int("$rc"), "still_attached": bool($attached),
  "data_identical": "$ref" == "$back", "c2c_kfix_loaded": bool($kfix)}))
EOF
)"
summary="1 GiB in slave RAM; link cut for $(python3 -c "print(round($tu-$tb,1))") s mid-read; read resumed, data $( [ "$ref" = "$back" ] && echo identical || echo DIFFERENT ), device $( [ $attached = 1 ] && echo still attached || echo LOST ); kfix loaded=$kfix"
if [ "$ref" = "$back" ] && [ "$rc" = 0 ] && [ $attached = 1 ] && [ "$st" = UP ] && [ $kfix = 1 ]; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary" "$metrics"
fi
