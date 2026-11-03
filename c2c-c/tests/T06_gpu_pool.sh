#!/bin/bash
# T06 GPU pool. Same plain CUDA program on both GPUs at once, started from the
# master (the slave's copy goes over the link). Per GPU + total: biggest usable
# allocation, FP32 FMA, cuBLAS SGEMM, tensor-core HGEMM, memory bandwidth.
# Slave GPU load also gets read from c2cd telemetry mid-run (no ssh).
# PASS: both GPUs finish, total allocation > either board alone.
#
set -uo pipefail
TEST_ID=T06; TEST_NAME="GPU pool (2x Volta, memory + compute)"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link

W=/var/tmp/c2c-t06; rm -rf "$W"; mkdir -p "$W"; samp=""
trap '[ -n "$samp" ] && kill $samp 2>/dev/null; rm -rf "$W"' EXIT
say "running gpu_probe --bench on both GPUs concurrently"
# peak GPU load on both boards (c2cd telemetry) while the bench runs
( echo 0 > "$W/gm"; echo 0 > "$W/gs"
  while [ ! -f "$W/done" ]; do
      link_json | python3 -c "
import json,sys; d=json.load(sys.stdin)
for k,f in (('local','$W/gm'),('peer','$W/gs')):
    v=d[k]['gpu_util']; o=float(open(f).read() or 0)
    open(f,'w').write(str(max(v,o)))"
      sleep 0.5
  done ) & samp=$!
"$BUILD/gpu_probe" --bench > "$W/m.json" 2>"$W/m.err" & pm=$!
c2crun "~/$PEER_BUILD/gpu_probe --bench" > "$W/s.json" 2>"$W/s.err" & ps=$!
wait $pm; rm_=$?; wait $ps; rs_=$?; touch "$W/done"; wait $samp

out="$(python3 - "$W" "$rm_" "$rs_" <<'EOF'
import json, sys, os
w = sys.argv[1]
def load(n):
    try: return json.load(open(f"{w}/{n}.json"))
    except Exception: return {}
m, s = load("m"), load("s")
rd = lambda f: float(open(f"{w}/{f}").read() or 0) if os.path.exists(f"{w}/{f}") else 0.0
G = lambda b: round(b / 2**30, 2)
keys = ["fp32_fma_tflops", "sgemm_tflops", "hgemm_tc_tflops", "membw_gbs"]
agg = {k: round(m.get(k, 0) + s.get(k, 0), 3) for k in keys}
agg.update({"gpus": int(bool(m)) + int(bool(s)), "sms": m.get("sms", 0) + s.get("sms", 0),
            "cuda_cores": m.get("cuda_cores", 0) + s.get("cuda_cores", 0),
            "tensor_cores": m.get("tensor_cores", 0) + s.get("tensor_cores", 0),
            "max_alloc_gib": G(m.get("max_alloc", 0) + s.get("max_alloc", 0))})
per = {n: {"host": d.get("host"), "name": d.get("name"), "max_alloc_gib": G(d.get("max_alloc", 0)),
           "gpu_total_mem_gib": G(d.get("total_mem", 0)), **{k: d.get(k, 0) for k in keys}}
       for n, d in (("master", m), ("slave", s))}
ok = (sys.argv[2] == sys.argv[3] == "0" and agg["gpus"] == 2 and
      agg["max_alloc_gib"] > max(per["master"]["max_alloc_gib"], per["slave"]["max_alloc_gib"]))
met = {"master": per["master"], "slave": per["slave"], "cluster": agg,
       "gpu_util_peak": {"master": rd("gm"), "slave": rd("gs")}}
print(json.dumps({"ok": ok, "metrics": met}))
print(f"2 GPUs together: {agg['max_alloc_gib']} GiB allocated ({per['master']['max_alloc_gib']}+{per['slave']['max_alloc_gib']}), "
      f"FP32 {agg['fp32_fma_tflops']} TFLOPS, SGEMM {agg['sgemm_tflops']} TFLOPS, tensor-core HGEMM {agg['hgemm_tc_tflops']} TFLOPS, "
      f"{agg['membw_gbs']} GB/s mem bandwidth; slave GPU {met['gpu_util_peak']['slave']}% peak load (c2cd telemetry)")
EOF
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    cat "$W/m.err" "$W/s.err" >&2
    result FAIL "$summary" "$metrics"
fi
