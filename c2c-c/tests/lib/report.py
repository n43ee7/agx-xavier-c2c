#!/usr/bin/env python3
"""Gathers the T*.json in a results dir into report.md + summary.json."""
import glob
import json
import os
import sys
import time

d = sys.argv[1]
res = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(d, "T*.json")))]
npass = sum(r["status"] == "PASS" for r in res)

lines = [f"# C2C proof report ({time.strftime('%Y-%m-%d %H:%M')})", "",
         f"**{npass}/{len(res)} tests passed.** Master = Xavier (16 GB), slave = xavier32 (32 GB), "
         "linked by C2C over USB-C. Every workload is an ordinary program with no C2C code in it.", "",
         "| test | result | summary |", "|---|---|---|"]
for r in res:
    lines.append(f"| {r['id']} {r['name']} | **{r['status']}** | {r['summary']} |")

inv = next((r for r in res if r["id"] == "T02"), None)
if inv:
    m, s, c = (inv["metrics"][k] for k in ("master", "slave", "cluster"))
    lines += ["", "## What the master can use", "",
              "| | master | slave | total |", "|---|---|---|---|",
              f"| CPU cores | {m['cpus']} x {m['cpu']} @ {m['cpu_max_mhz']} MHz | {s['cpus']} | **{c['cpus']}** |",
              f"| RAM | {m['ram_gib']} GiB | {s['ram_gib']} GiB | **{c['ram_gib']} GiB** |",
              f"| GPU | {m['gpu']} (cc {m['gpu_cc']}) | {s['gpu']} | **{c['gpus']} GPUs** |",
              f"| SMs / CUDA cores / tensor cores | {m['gpu_sms']} / {m['cuda_cores']} / {m['tensor_cores']} | "
              f"{s['gpu_sms']} / {s['cuda_cores']} / {s['tensor_cores']} | "
              f"**{c['gpu_sms']} / {c['cuda_cores']} / {c['tensor_cores']}** |",
              f"| GPU memory (unified) | {m['gpu_mem_gib']} GiB | {s['gpu_mem_gib']} GiB | **{c['gpu_mem_gib']} GiB** |",
              f"| peak FP32 | {m['peak_fp32_tflops']} TFLOPS | {s['peak_fp32_tflops']} TFLOPS | "
              f"**{c['peak_fp32_tflops']} TFLOPS** |",
              f"| NVENC / NVDEC / DLA | | | {c['nvenc']} / {c['nvdec']} / {c['dla']} |"]

lines += ["", "## Raw metrics", ""]
for r in res:
    lines += [f"### {r['id']} {r['name']} ({r['status']}, {r['time']})", "", "```json",
              json.dumps(r["metrics"], indent=2), "```", ""]

open(os.path.join(d, "report.md"), "w").write("\n".join(lines) + "\n")
json.dump({"passed": npass, "total": len(res),
           "tests": [{k: r[k] for k in ("id", "name", "status", "summary")} for r in res]},
          open(os.path.join(d, "summary.json"), "w"), indent=2)
print(f"{npass}/{len(res)} passed")
for r in res:
    print(f"  {r['id']} {r['status']:4}  {r['summary']}")
print(f"report: {os.path.join(d, 'report.md')}")
