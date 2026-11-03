#!/bin/bash
# T04 remote paging. A plain program capped at 2 GiB (memory cgroup) chews
# through 8 GiB. Whatever doesn't fit gets paged to the SLAVE's RAM over C2C
# (c2c-rmem swap at prio 100, ahead of zram), read back and verified.
# The cap keeps the rest of the master out of it, so this is the safe,
# repeatable version of the RAM test.
# PASS: 0 verify errors, >= 4 GiB sitting in the slave's RAM at peak.
set -uo pipefail
TEST_ID=T04; TEST_NAME="RAM pool: remote paging (2 GiB cap, 8 GiB program)"
source "$(dirname "$0")/lib/common.sh"
source "$TESTS_DIR/lib/rmem.sh"
require_root; require_link

GIB="${C2C_T04_GIB:-8}"
CAP="${C2C_T04_CAP:-2G}"
say "lending 12 GiB of slave RAM to the master"
mkdir -p "$RESULTS"
rmem_start 12G || { result FAIL "could not attach remote memory"; exit 1; }

say "running mem_proof -g $GIB inside a $CAP memory cap"
out="$(systemd-run --quiet --scope -p MemoryLimit="$CAP" "$BUILD/mem_proof" -g "$GIB" 2>"$RESULTS/$TEST_ID.log")"
rc=$?
rmem_stop_sampler
read -r pk_remote pk_zram pk_memd <<<"$(rmem_peaks)"
rmem_cleanup

metrics="$(python3 - "$out" "$pk_remote" "$pk_zram" "$pk_memd" "$CAP" "$rc" <<'EOF'
import json, sys
o = json.loads(sys.argv[1]) if sys.argv[1].strip() else {}
r, z, m = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
o.update({"ram_cap": sys.argv[5], "exit_code": int(sys.argv[6]),
          "peak_remote_swap_gib": round(r / 2**20, 2), "peak_zram_gib": round(z / 2**20, 2),
          "peak_slave_memd_resident_gib": round(m / 2**30, 2)})
print(json.dumps(o))
EOF
)"
read -r errs wr vr rem memd <<<"$(python3 -c 'import json,sys; o=json.loads(sys.argv[1]); print(o.get("errors",-1), o.get("write_gib_s",0), o.get("verify_gib_s",0), o["peak_remote_swap_gib"], o["peak_slave_memd_resident_gib"])' "$metrics")"
summary="${GIB} GiB program capped at $CAP RAM: ${rem} GiB paged into slave RAM (memd resident ${memd} GiB), write ${wr} GiB/s, verify ${vr} GiB/s, ${errs} errors"
if [ "$rc" = 0 ] && [ "$errs" = 0 ] && python3 -c "import sys; sys.exit(0 if $rem >= 4 else 1)"; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary (exit $rc)" "$metrics"
fi
