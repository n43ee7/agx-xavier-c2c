#!/bin/bash
# T09 one process, both GPUs. A normal program on the master drives its own
# GPU and the slave's at the same time, like a 2-GPU box:
#   1. gpu_check --all: one SAXPY over 256 Mi elements split over both GPUs
#      (verified), then the full check suite on each GPU
#   2. NVIDIA's simpleMultiGPU + MonteCarloMultiGPU samples with both GPUs
#      visible (they check their own results)
# PASS: everything correct on both GPUs.
set -uo pipefail
TEST_ID=T09; TEST_NAME="one process uses both GPUs"
source "$(dirname "$0")/lib/common.sh"
source "$TESTS_DIR/lib/cuda_samples.sh"
require_root; require_link; require_remote_gpu

W=/var/tmp/c2c-t09; rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
SAMPLE_LOG_DIR="$W"
say "gpu_check --all (both GPUs in one process)"
c2c-gpu run "$BUILD/gpu_check" --all > "$W/all.json" 2> "$W/all.err"; rc=$?
say "NVIDIA multi-GPU samples"
s1="$(run_sample all simpleMultiGPU)"; s2="$(run_sample all MonteCarloMultiGPU -method=threaded)"
s3="$(run_sample all deviceQuery)"
grep -o "Detected [0-9]* CUDA Capable device(s)" "$W/all.deviceQuery.log" > "$W/dq" 2>/dev/null

out="$(python3 - "$W" "$rc" "$s1" "$s2" "$s3" <<'PY'
import json, sys
w, rc, s1, s2, s3 = sys.argv[1], int(sys.argv[2]), *sys.argv[3:6]
try: a = json.load(open(f"{w}/all.json"))
except Exception: a = {}
devs = a.get("devices", [])
split = a.get("split", {})
samples = {x.split()[2]: x.split()[0] for x in (s1, s2, s3) if len(x.split()) == 3}
dq = open(f"{w}/dq").read().strip()
ok = rc == 0 and a.get("ok") and len(devs) == 2 and split.get("ok") and all(v == "PASS" for v in samples.values()) and len(samples) == 3
met = {"gpus": a.get("gpus"), "split_saxpy": split,
       "per_gpu": [{k: d.get(k) for k in ("device", "pci_domain", "ok", "saxpy_ok", "sgemm_ok", "streams_ok",
                                           "htod_pinned_mbs", "async_launches_per_s")} for d in devs],
       "samples": samples, "deviceQuery": dq}
print(json.dumps({"ok": bool(ok), "metrics": met}))
print(f"one process, {a.get('gpus')} GPUs: split SAXPY over {split.get('elements')} elements on {split.get('devices')} GPUs ok={split.get('ok')} "
      f"({split.get('seconds')} s); per-GPU checks {[d.get('ok') for d in devs]}; samples {samples}; deviceQuery: {dq}")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    tail -3 "$W/all.err" >&2
    result FAIL "$summary" "$metrics"
fi
