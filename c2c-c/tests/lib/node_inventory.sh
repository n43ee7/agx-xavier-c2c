#!/bin/bash
# Dumps one board's hardware inventory as JSON. Run it locally, or pipe it
# through c2crun to run on the peer:  node_inventory.sh <path-to-gpu_probe>
gp="${1:-gpu_probe}"
cpu_max_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo 0)
model=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || uname -m)
gpu=$("$gp" --info 2>/dev/null || echo '{}')
python3 - "$cpu_max_khz" "$model" "$gpu" <<'EOF'
import json, os, sys
khz, model, gpu = sys.argv[1], sys.argv[2], sys.argv[3]
mem = {l.split(':')[0]: int(l.split()[1]) for l in open('/proc/meminfo') if l.split()[1].isdigit()}
cpu_part = ''
for l in open('/proc/cpuinfo'):
    if l.startswith('CPU part'): cpu_part = l.split(':')[1].strip(); break
print(json.dumps({
    "host": os.uname().nodename, "model": model,
    "cpus": os.cpu_count(), "cpu_part": cpu_part,
    "cpu_name": {"0x004": "NVIDIA Carmel (ARMv8.2)"}.get(cpu_part, "ARMv8 " + cpu_part),
    "cpu_max_mhz": int(khz) // 1000,
    "mem_total_kb": mem.get("MemTotal", 0), "mem_avail_kb": mem.get("MemAvailable", 0),
    "swap_total_kb": mem.get("SwapTotal", 0),
    "gpu": json.loads(gpu)}))
EOF
