#!/bin/bash
# T11 remote GPU over TCP vs a link outage. gpu_check runs on the slave's GPU
# with gpu_transport=tcp (kernels, cuBLAS, streams, copies, all verified).
# ~3 s in we restart the master's c2cd: link DOWN, c2c0 torn down and rebuilt,
# handshake again. The TCP connection to c2c-gpud just stalls and resumes, and
# the program should finish with correct results, seeing nothing but a pause.
# (raw USB channels don't go through c2cd at all, T13 cuts the USB itself.)
#
# PASS: gpu_check exit 0, all checks verified, link UP again.
set -uo pipefail
TEST_ID=T11; TEST_NAME="remote GPU (TCP) survives link outage"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link; require_remote_gpu

W=/var/tmp/c2c-t11; rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
say "gpu_check on the remote GPU; c2cd restart (link cut) after 3 s"
t0=$(now)
C2C_GPU_TRANSPORT=tcp CUDA_VISIBLE_DEVICES=1 C2C_GPU_STATS=1 c2c-gpu run "$BUILD/gpu_check" > "$W/check.json" 2> "$W/check.err" &
pid=$!
sleep 3
tb=$(now); systemctl restart c2cd
for i in $(seq 1 60); do
    st=$(link_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])' 2>/dev/null)
    [ "$st" = UP ] && break; sleep 0.5
done
tu=$(now)
wait $pid; rc=$?; t1=$(now)

out="$(python3 - "$W" "$rc" "$tb" "$tu" "$t0" "$t1" "${st:-?}" <<'PY'
import json, sys
w, rc, tb, tu, t0, t1, st = sys.argv[1], int(sys.argv[2]), *map(float, sys.argv[3:7]), sys.argv[7]
try: c = json.load(open(f"{w}/check.json"))
except Exception: c = {}
d = (c.get("devices") or [{}])[0]
checks = ["saxpy_ok", "reduction_ok", "sgemm_ok", "struct_param_ok", "streams_ok"]
ok = rc == 0 and all(d.get(k) for k in checks) and st == "UP" and d.get("pci_domain") == 0xc2c
met = {"exit_code": rc, "link_outage_s": round(tu - tb, 2), "run_s": round(t1 - t0, 2),
       "link_state_after": st, "checks": {k: d.get(k) for k in checks}, "ran_on_remote": d.get("pci_domain") == 0xc2c}
print(json.dumps({"ok": ok, "metrics": met}))
print(f"remote GPU job ran through a {round(tu - tb, 1)} s link cut: exit {rc}, results verified={all(d.get(k) for k in checks)}, "
      f"total {round(t1 - t0, 1)} s, link {st}")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    tail -3 "$W/check.err" >&2
    result FAIL "$summary" "$metrics"
fi
