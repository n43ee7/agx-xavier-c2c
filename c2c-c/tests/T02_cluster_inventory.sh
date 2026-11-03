#!/bin/bash
# T02 inventory: what the master can actually see/use through C2C.
# Same plain probe on both boards (CPU, RAM, GPU via CUDA), the peer's one
# runs over the link. Then c2cd's telemetry (comes over the link's control
# channel, no ssh) gets cross-checked against it.
# PASS: both boards answer, both GPUs show up, telemetry agrees on CPUs/RAM.
set -uo pipefail
TEST_ID=T02; TEST_NAME="cluster inventory (CPUs, RAM, GPUs)"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link

say "probing master"
loc="$(bash "$TESTS_DIR/lib/node_inventory.sh" "$BUILD/gpu_probe")"
say "probing slave over c2c"
rem="$(c2crun "bash -s ~/$PEER_BUILD/gpu_probe" < "$TESTS_DIR/lib/node_inventory.sh")"
tel="$(link_json)"

out="$(python3 - "$loc" "$rem" "$tel" <<'EOF'
import json, sys
L, R, T = (json.loads(x) for x in sys.argv[1:])
gib = lambda kb: kb / 1048576
def row(n):
    g = n["gpu"]
    return {"host": n["host"], "cpus": n["cpus"], "cpu": n["cpu_name"], "cpu_max_mhz": n["cpu_max_mhz"],
            "ram_gib": round(gib(n["mem_total_kb"]), 2), "ram_avail_gib": round(gib(n["mem_avail_kb"]), 2),
            "gpu": g.get("name"), "gpu_cc": g.get("cc"), "gpu_sms": g.get("sms", 0),
            "cuda_cores": g.get("cuda_cores", 0), "tensor_cores": g.get("tensor_cores", 0),
            "gpu_clock_mhz": g.get("clock_mhz", 0), "gpu_mem_gib": round(g.get("total_mem", 0) / 2**30, 2),
            "peak_fp32_tflops": g.get("peak_fp32_tflops", 0)}
m, s = row(L), row(R)
P = T["peer"]
tot = {"nodes": 2, "cpus": m["cpus"] + s["cpus"], "ram_gib": round(m["ram_gib"] + s["ram_gib"], 2),
       "ram_avail_gib": round(m["ram_avail_gib"] + s["ram_avail_gib"], 2),
       "gpus": int(bool(m["gpu"])) + int(bool(s["gpu"])), "gpu_sms": m["gpu_sms"] + s["gpu_sms"],
       "cuda_cores": m["cuda_cores"] + s["cuda_cores"], "tensor_cores": m["tensor_cores"] + s["tensor_cores"],
       "gpu_mem_gib": round(m["gpu_mem_gib"] + s["gpu_mem_gib"], 2),
       "peak_fp32_tflops": round(m["peak_fp32_tflops"] + s["peak_fp32_tflops"], 3),
       "nvenc": T["local"]["nvenc"] + P["nvenc"], "nvdec": T["local"]["nvdec"] + P["nvdec"],
       "dla": T["local"]["dla"] + P["dla"]}
tel_ok = P["ncpu"] == s["cpus"] and abs(P["mem_total_kb"] - R["mem_total_kb"]) < 65536
ok = tot["gpus"] == 2 and tel_ok and s["cpus"] > 0
print(json.dumps({"ok": ok, "metrics": {"master": m, "slave": s, "cluster": tot,
                  "telemetry_matches_probe": tel_ok}}))
print(f"{tot['cpus']} CPUs ({m['cpus']}+{s['cpus']}), {tot['ram_gib']} GiB RAM ({m['ram_gib']}+{s['ram_gib']}), "
      f"{tot['gpus']} GPUs / {tot['gpu_sms']} SMs / {tot['cuda_cores']} CUDA cores / {tot['tensor_cores']} tensor cores, "
      f"peak {tot['peak_fp32_tflops']} FP32 TFLOPS")
EOF
)"
json="$(sed -n 1p <<<"$out")"; summary="$(sed -n 2p <<<"$out")"
metrics="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["metrics"]))' "$json")"
if python3 -c 'import json,sys; sys.exit(0 if json.loads(sys.argv[1])["ok"] else 1)' "$json"; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary" "$metrics"
fi
