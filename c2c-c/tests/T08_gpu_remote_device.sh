#!/bin/bash
# T08 slave GPU = CUDA device on the master. A normal CUDA program started
# with `c2c-gpu run` sees 2 GPUs: 0 = master's Xavier, 1 = slave's (over the
# link). With CUDA_VISIBLE_DEVICES=1 it runs only on the slave's GPU.
#   1. c2c-gpu ls: 2 devices, one remote, slave's memory size
#   2. gpu_check on the remote GPU: kernels, atomics, __device__ symbols,
#      structs by value, cuBLAS SGEMM vs a CPU reference, 4-stream pipeline,
#      all verified. Also link bandwidth + launch/sync latency
#   3. gpu_probe --bench (cuBLAS SGEMM, tensor-core HGEMM, FP32, mem
#      bandwidth, 20 GiB alloc) from the master vs natively on the slave
#
# PASS: all checks correct; remote compute >= 95% of native.
set -uo pipefail
TEST_ID=T08; TEST_NAME="remote GPU as a local CUDA device"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link; require_remote_gpu

W=/var/tmp/c2c-t08; rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
gpu_list > "$W/ls.json"
say "GPUs visible to a program on the master:"; cat "$W/ls.json" >&2

say "gpu_check on the remote GPU (unmodified program, results verified)"
CUDA_VISIBLE_DEVICES=1 C2C_GPU_STATS=1 c2c-gpu run "$BUILD/gpu_check" > "$W/check.json" 2> "$W/check.err"
rc_check=$?
say "gpu_probe --bench natively on the slave"
c2crun "~/$PEER_BUILD/gpu_probe --bench" > "$W/native.json" 2>"$W/native.err"
say "gpu_probe --bench on the remote GPU from the master"
CUDA_VISIBLE_DEVICES=1 c2c-gpu run "$BUILD/gpu_probe" --bench > "$W/remote.json" 2> "$W/remote.err"
rc_probe=$?

out="$(python3 - "$W" "$rc_check" "$rc_probe" <<'PY'
import json, sys, re
w, rc_check, rc_probe = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
m = re.search(r"remote GPU via ([^\s:]+)", open(f"{w}/check.err").read())
via = m.group(1) if m else "?"
def load(n):
    try: return json.load(open(f"{w}/{n}.json"))
    except Exception: return {}
ls = json.load(open(f"{w}/ls.json")); chk = load("check"); nat = load("native"); rem = load("remote")
dev = (chk.get("devices") or [{}])[0]
keys = ["fp32_fma_tflops", "sgemm_tflops", "hgemm_tc_tflops", "membw_gbs"]
ratio = {k: round(rem.get(k, 0) / nat[k], 3) if nat.get(k) else 0 for k in keys}
remote = [g for g in ls if g["remote"]]
checks = ["saxpy_ok", "reduction_ok", "sgemm_ok", "struct_param_ok", "streams_ok"]
ok = (len(ls) == 2 and len(remote) == 1 and rc_check == 0 and all(dev.get(c) for c in checks)
      and rc_probe == 0 and all(r >= 0.95 for r in ratio.values()))
met = {"gpus": ls, "checks": {c: dev.get(c) for c in checks},
       "link": {k: dev.get(k) for k in ["htod_pinned_mbs", "dtoh_pinned_mbs", "htod_pageable_mbs",
                                         "dtoh_pageable_mbs", "async_launches_per_s", "launch_sync_us", "dtoh_4b_us"]},
       "native_on_slave": {k: nat.get(k) for k in keys + ["max_alloc"]},
       "remote_from_master": {k: rem.get(k) for k in keys + ["max_alloc"]},
       "remote_vs_native": ratio, "transport": via}
print(json.dumps({"ok": ok, "metrics": met}))
g = remote[0] if remote else {}
print(f"master sees {len(ls)} GPUs (remote: {g.get('name')} {round(g.get('total_mem', 0) / 2**30, 1)} GiB); "
      f"remote results verified={all(dev.get(c) for c in checks)}; remote/native: SGEMM {rem.get('sgemm_tflops')}/{nat.get('sgemm_tflops')} TFLOPS, "
      f"HGEMM {rem.get('hgemm_tc_tflops')}/{nat.get('hgemm_tc_tflops')}, membw {rem.get('membw_gbs')}/{nat.get('membw_gbs')} GB/s, "
      f"alloc {round(rem.get('max_alloc', 0) / 2**30, 1)} GiB; link {dev.get('htod_pinned_mbs')}/{dev.get('dtoh_pinned_mbs')} MB/s, "
      f"{dev.get('async_launches_per_s')} launches/s, sync {dev.get('launch_sync_us')} us, via {via}")
PY
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    tail -3 "$W/check.err" "$W/remote.err" >&2
    result FAIL "$summary" "$metrics"
fi
