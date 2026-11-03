#!/bin/bash
# T12 raw USB channels = the remote GPU's data path with no TCP/IP or c2cd.
#   1. c2c-gpu-bench over a raw channel, then over TCP, back to back:
#      upload, download, both at once, sync round trip, 4-byte read,
#      GPU0 <-> GPU1 copies
#   2. channels+1 programs at once: each grabs a raw channel and the extra
#      one has to fall back to TCP. all of them must work
# PASS: raw up/down >= 440 MB/s (USB 3 Gen 1 tops out ~470), both at once
#       >= 750 MB/s (link is full duplex), raw sync p50 <= 70% of TCP's,
#       GPU0<->GPU1 >= 400 MB/s, concurrent programs all OK on the
#       transports we expect.
set -uo pipefail
TEST_ID=T12; TEST_NAME="raw USB channels (TCP-free GPU data path)"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link; require_remote_gpu

W=/var/tmp/c2c-t12; rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
nch=$(ls /dev/c2cch* 2>/dev/null | wc -l)
[ "$nch" -gt 0 ] || { result FAIL "no raw channels (/dev/c2cch*): is raw_channels > 0 on the slave?"; exit 1; }
BENCH="$(command -v c2c-gpu-bench || echo /opt/c2c/bin/c2c-gpu-bench)"

say "bench over a raw channel"
C2C_GPU_TRANSPORT=raw C2C_GPU_STATS=1 c2c-gpu run "$BENCH" -m 256 -n 2000 --json > "$W/raw.json" 2> "$W/raw.err"
say "bench over TCP"
C2C_GPU_TRANSPORT=tcp C2C_GPU_STATS=1 c2c-gpu run "$BENCH" -m 256 -n 2000 --json > "$W/tcp.json" 2> "$W/tcp.err"

say "$((nch + 1)) programs at once ($nch raw channels)"
for i in $(seq 0 "$nch"); do
    C2C_GPU_STATS=1 c2c-gpu run "$BENCH" -m 32 -n 300 --json > "$W/c$i.json" 2> "$W/c$i.err" &
done
wait

out="$(python3 - "$W" "$nch" <<'PY'
import json, sys, glob, re
w, nch = sys.argv[1], int(sys.argv[2])
def load(n):
    try: return json.load(open(f"{w}/{n}.json"))
    except Exception: return {}
def via(n):
    m = re.search(r"remote GPU via ([^\s:]+)", open(f"{w}/{n}.err").read())
    return m.group(1) if m else "?"
r, t = load("raw"), load("tcp")
conc = [load(f"c{i}") for i in range(nch + 1)]
vias = [via(f"c{i}") for i in range(nch + 1)]
n_raw = sum(v.startswith("/dev/c2cch") for v in vias); n_tcp = sum(v == "tcp" for v in vias)
ok_conc = all(c.get("h2d_mbs", 0) > 0 for c in conc) and n_raw == nch and n_tcp == 1
ok = (via("raw").startswith("/dev/c2cch") and r.get("h2d_mbs", 0) >= 440 and r.get("d2h_mbs", 0) >= 440
      and r.get("bidir_mbs", 0) >= 750 and r.get("sync_p50_us", 1e9) <= 0.7 * t.get("sync_p50_us", 0)
      and min(r.get("peer_to_mbs", 0), r.get("peer_from_mbs", 0)) >= 400 and ok_conc)
keys = ["h2d_mbs", "d2h_mbs", "bidir_mbs", "sync_p50_us", "sync_p99_us", "read4_p50_us", "peer_to_mbs", "peer_from_mbs"]
met = {"raw": {k: r.get(k) for k in keys}, "tcp": {k: t.get(k) for k in keys}, "channels": nch,
       "concurrent": {"programs": nch + 1, "raw": n_raw, "tcp": n_tcp, "all_ok": ok_conc,
                      "h2d_mbs": [c.get("h2d_mbs") for c in conc]}}
print(json.dumps({"ok": ok, "metrics": met}))
print(f"raw: up {r.get('h2d_mbs')} / down {r.get('d2h_mbs')} / both {r.get('bidir_mbs')} MB/s, sync p50 {r.get('sync_p50_us')} us "
      f"(TCP {t.get('sync_p50_us')} us), 4B read p50 {r.get('read4_p50_us')} us (TCP {t.get('read4_p50_us')}), "
      f"GPU0<->GPU1 {r.get('peer_to_mbs')}/{r.get('peer_from_mbs')} MB/s; {nch + 1} programs at once: "
      f"{n_raw} raw + {n_tcp} TCP, all ok={ok_conc}")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    tail -3 "$W/raw.err" "$W/tcp.err" >&2
    result FAIL "$summary" "$metrics"
fi
