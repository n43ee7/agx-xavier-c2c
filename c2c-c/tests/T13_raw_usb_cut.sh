#!/bin/bash
# T13 actual USB cut in the middle of a raw-channel GPU session. gpu_check
# runs on the remote GPU over a raw channel, 3 s in the master de-authorizes
# the USB device for 1.5 s (bus drops: link, c2c0, every channel gone, then
# re-enumerated).
# A raw channel can't survive that (TCP over c2c0 can, see T11). What we need:
#   - no hang: the program errors out within 30 s
#   - the link comes back UP on its own
#   - a new raw session runs at full speed (USB controllers still healthy)
# PASS: all three.
set -uo pipefail
TEST_ID=T13; TEST_NAME="raw channel: clean failure and recovery after a USB cut"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link; require_remote_gpu

W=/var/tmp/c2c-t13; rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
dev=""
for d in /sys/bus/usb/devices/*; do [ "$(cat $d/idVendor 2>/dev/null)" = 1209 ] && dev="$d"; done
[ -n "$dev" ] || { result FAIL "C2C USB device not found"; exit 1; }
BENCH="$(command -v c2c-gpu-bench || echo /opt/c2c/bin/c2c-gpu-bench)"

say "gpu_check over a raw channel; USB cut ($(basename $dev) de-authorized 1.5 s) after 3 s"
t0=$(now)
C2C_GPU_TRANSPORT=raw CUDA_VISIBLE_DEVICES=1 C2C_GPU_STATS=1 timeout 120 c2c-gpu run "$BUILD/gpu_check" > "$W/check.json" 2> "$W/check.err" &
pid=$!
sleep 3
echo 0 > "$dev/authorized"; sleep 1.5; echo 1 > "$dev/authorized"
tc=$(now)
ended=-1
for i in $(seq 1 300); do kill -0 $pid 2>/dev/null || { ended=1; break; }; sleep 0.1; done
wait $pid 2>/dev/null; rc=$?
te=$(now)
for i in $(seq 1 120); do
    st=$(link_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])' 2>/dev/null)
    [ "$st" = UP ] && [ -e /dev/c2cch0 ] && break; sleep 0.5
done
tu=$(now)
sleep 3   # c2c-gpud picks the new channels up within 2 s
say "new raw session after the cut"
C2C_GPU_TRANSPORT=raw C2C_GPU_STATS=1 c2c-gpu run "$BENCH" -m 128 -n 500 --json > "$W/after.json" 2> "$W/after.err"; rc2=$?

out="$(python3 - "$W" "$rc" "$ended" "$tc" "$te" "$tu" "${st:-?}" "$rc2" <<'PY'
import json, sys, re
w, rc, ended = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
tc, te, tu = map(float, sys.argv[4:7]); st, rc2 = sys.argv[7], int(sys.argv[8])
try: a = json.load(open(f"{w}/after.json"))
except Exception: a = {}
via = re.search(r"remote GPU via ([^\s:]+)", open(f"{w}/after.err").read())
via = via.group(1) if via else "?"
no_hang = ended == 1 and rc != 124
ok = no_hang and st == "UP" and rc2 == 0 and via.startswith("/dev/c2cch") and a.get("h2d_mbs", 0) >= 440 and a.get("d2h_mbs", 0) >= 440
met = {"first_program_exit": rc, "ended_after_cut_s": round(te - tc, 1), "no_hang": no_hang, "link_state": st,
       "link_back_s": round(tu - tc, 1), "after": {k: a.get(k) for k in ["h2d_mbs", "d2h_mbs", "bidir_mbs", "sync_p50_us"]},
       "after_via": via}
print(json.dumps({"ok": ok, "metrics": met}))
print(f"USB cut 1.5 s: running program ended in {round(te - tc, 1)} s with exit {rc} (no hang={no_hang}); link {st} after "
      f"{round(tu - tc, 1)} s; new raw session {via}: up {a.get('h2d_mbs')} / down {a.get('d2h_mbs')} MB/s, sync p50 {a.get('sync_p50_us')} us")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    tail -5 "$W/check.err" "$W/after.err" >&2
    result FAIL "$summary" "$metrics"
fi
