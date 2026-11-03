#!/bin/bash
# Runs the proof tests (on the MASTER) and writes a report.
#   sudo C2C_PEER_PASS=<slave pw> tests/run_all.sh    all tests (or type the password)
#   sudo tests/run_all.sh T02 T06    selected tests
# results land in tests/results/<timestamp>/{T*.json, report.md, summary.json}
set -uo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0"; exit 2; }
[ -x build/mem_proof ] && [ -x build/gpu_probe ] || { echo "run tests/setup_tests.sh first"; exit 2; }

ts=$(date +%Y%m%d-%H%M%S)
export C2C_RESULTS="$PWD/results/$ts"
mkdir -p "$C2C_RESULTS"
sel=("$@")
[ ${#sel[@]} = 0 ] && sel=(T01 T02 T03 T04 T05 T06 T07 T08 T09 T10 T11 T12 T13)

fails=0
for t in "${sel[@]}"; do
    f=$(ls ${t}_*.sh 2>/dev/null | head -1)
    [ -n "$f" ] || { echo "no test $t"; continue; }
    echo; echo "=== $f"
    bash "./$f" || fails=$((fails + 1))
done
echo
python3 lib/report.py "$C2C_RESULTS"
TEST_ID=run; source lib/common.sh; c2crun_close
ln -sfn "$ts" results/latest
fix_owner results
exit $fails
