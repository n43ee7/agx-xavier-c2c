#!/bin/bash
# T10 stock NVIDIA CUDA samples on the remote GPU. ~50 of them (runtime +
# driver API, cuBLAS, cuFFT, cuRAND, cuSPARSE, streams, events, textures,
# cooperative groups, atomics, warp intrinsics...), each checks itself.
# They all run locally through the same shim too, to make sure the local
# GPU isn't broken by it.
# PASS: nothing fails remotely that passes locally. WAIVED = the sample says
# a feature is missing (e.g. unified memory, which the remote GPU doesn't have).
#
set -uo pipefail
TEST_ID=T10; TEST_NAME="NVIDIA CUDA samples on the remote GPU"
source "$(dirname "$0")/lib/common.sh"
source "$TESTS_DIR/lib/cuda_samples.sh"
require_root; require_link; require_remote_gpu
[ -d "$SAMPLES_DIR" ] || { result FAIL "samples not built (run setup_tests.sh)"; exit 1; }

SAMPLE_LOG_DIR="$RESULTS/T10.logs"; mkdir -p "$SAMPLE_LOG_DIR"
say "running every sample locally (through the shim) and on the remote GPU"
run_samples local > "$RESULTS/T10.local.txt"
run_samples remote > "$RESULTS/T10.remote.txt"
fix_owner "$RESULTS"

out="$(python3 - "$RESULTS/T10.local.txt" "$RESULTS/T10.remote.txt" <<'PY'
import json, sys
def load(p):
    r = {}
    for l in open(p):
        f = l.split()
        if len(f) == 3: r[f[2]] = (f[0], float(f[1]))
    return r
loc, rem = load(sys.argv[1]), load(sys.argv[2])
count = lambda d, s: sum(1 for v in d.values() if v[0] == s)
regress = sorted(n for n, v in rem.items() if v[0] == "FAIL" and loc.get(n, ("",))[0] == "PASS")
local_fail = sorted(n for n, v in loc.items() if v[0] == "FAIL")
ok = not regress and not local_fail and count(rem, "PASS") > 0
met = {"samples": len(rem), "remote": {s: count(rem, s) for s in ("PASS", "WAIVED", "FAIL")},
       "local": {s: count(loc, s) for s in ("PASS", "WAIVED", "FAIL")},
       "remote_failures_that_pass_locally": regress, "local_failures": local_fail,
       "waived_remote": sorted(n for n, v in rem.items() if v[0] == "WAIVED"),
       "per_sample": {n: {"local": loc.get(n, ("?", 0))[0], "remote": v[0], "remote_s": v[1],
                          "local_s": loc.get(n, ("?", 0))[1]} for n, v in sorted(rem.items())}}
print(json.dumps({"ok": ok, "metrics": met}))
print(f"{len(rem)} unmodified samples: remote {met['remote']['PASS']} pass / {met['remote']['WAIVED']} waived / "
      f"{met['remote']['FAIL']} fail; local {met['local']['PASS']} pass / {met['local']['WAIVED']} waived / {met['local']['FAIL']} fail; "
      f"remote-only failures: {regress or 'none'}; waived remotely: {met['waived_remote']}")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary" "$metrics"
fi
