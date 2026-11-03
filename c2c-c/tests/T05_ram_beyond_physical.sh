#!/bin/bash
# T05 more RAM than the master has. ONE normal process allocates and uses
# more than the master's physical RAM (RAM + 6 GiB by default). The overflow
# lives in the SLAVE's RAM via c2c-rmem, every byte gets checked.
# PASS: footprint > master physical RAM, 0 verify errors, and the overflow
#       actually went to the slave (remote swap > local zram).
set -uo pipefail
TEST_ID=T05; TEST_NAME="RAM pool: one program larger than master RAM"
source "$(dirname "$0")/lib/common.sh"
source "$TESTS_DIR/lib/rmem.sh"
require_root; require_link

mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
EXTRA="${C2C_T05_EXTRA_GIB:-6}"
GIB=$(python3 -c "print(round($mem_kb / 2**20 + $EXTRA, 1))")
LEND="${C2C_T05_LEND:-16G}"
say "master physical RAM $(python3 -c "print(round($mem_kb/2**20,2))") GiB; program will use $GIB GiB"
say "lending $LEND of slave RAM to the master"
mkdir -p "$RESULTS"
rmem_start "$LEND" || { result FAIL "could not attach remote memory"; exit 1; }

say "running mem_proof -g $GIB (no cap; progress in $RESULTS/$TEST_ID.log)"
out="$("$BUILD/mem_proof" -g "$GIB" 2>"$RESULTS/$TEST_ID.log")"
rc=$?
rmem_stop_sampler
read -r pk_remote pk_zram pk_memd <<<"$(rmem_peaks)"
rmem_cleanup

metrics="$(python3 - "$out" "$pk_remote" "$pk_zram" "$pk_memd" "$mem_kb" "$rc" <<'EOF'
import json, sys
o = json.loads(sys.argv[1]) if sys.argv[1].strip() else {}
r, z, m, mk = (int(x) for x in sys.argv[2:6])
o.update({"master_physical_gib": round(mk / 2**20, 2), "exit_code": int(sys.argv[6]),
          "peak_remote_swap_gib": round(r / 2**20, 2), "peak_zram_gib": round(z / 2**20, 2),
          "peak_slave_memd_resident_gib": round(m / 2**30, 2),
          "footprint_over_physical_gib": round(o.get("gib", 0) - mk / 2**20, 2)})
print(json.dumps(o))
EOF
)"
read -r errs wr vr rem zr over <<<"$(python3 -c 'import json,sys; o=json.loads(sys.argv[1]); print(o.get("errors",-1), o.get("write_gib_s",0), o.get("verify_gib_s",0), o["peak_remote_swap_gib"], o["peak_zram_gib"], o["footprint_over_physical_gib"])' "$metrics")"
summary="one process used ${GIB} GiB on a $(python3 -c "print(round($mem_kb/2**20,1))") GiB master (+${over} GiB over physical): ${rem} GiB held in slave RAM, zram ${zr} GiB, write ${wr} GiB/s, verify ${vr} GiB/s, ${errs} errors"
if [ "$rc" = 0 ] && [ "$errs" = 0 ] && python3 -c "import sys; sys.exit(0 if $over > 0 and $rem > $zr else 1)"; then
    result PASS "$summary" "$metrics"
else
    result FAIL "$summary (exit $rc)" "$metrics"
fi
