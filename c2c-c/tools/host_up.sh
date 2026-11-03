#!/bin/bash
# master (USB host end): load c2c_host.ko and wait for /dev/c2c0 to show up (sudo)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
lsmod | grep -q '^c2c_host ' || insmod "$DIR/../kernel/host/c2c_host.ko" "$@"
for i in $(seq 1 50); do [ -e /dev/c2c0 ] && break; sleep 0.2; done
[ -e /dev/c2c0 ] && ls -l /dev/c2c0 || { echo "no /dev/c2c0 (is the gadget up on xavier32?)"; exit 1; }
