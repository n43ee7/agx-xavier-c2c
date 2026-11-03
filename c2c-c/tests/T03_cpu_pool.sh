#!/bin/bash
# T03 CPU pool. Same batch job (plain xz) twice:
#   A) master only, 8 workers on its 8 cores
#   B) pooled, 8 on the master + 8 on the slave (slave's in/out streams over the link)
# B's output gets decompressed and compared bit for bit with the input.
#
# PASS: pooled run verified and >= 1.6x faster than master-only.
set -uo pipefail
TEST_ID=T03; TEST_NAME="CPU pool (16 cores, unmodified xz)"
source "$(dirname "$0")/lib/common.sh"
require_root; require_link

DATA_MB="${C2C_T03_MB:-512}"
CHUNK_MB=8
WORK=/var/tmp/c2c-t03
rm -rf "$WORK"; mkdir -p "$WORK/in" "$WORK/a" "$WORK/b"
trap 'rm -rf "$WORK"' EXIT

say "building a ${DATA_MB} MiB dataset of real system binaries"
tar -cf - /usr/lib/aarch64-linux-gnu 2>/dev/null | head -c $((DATA_MB << 20)) > "$WORK/data"
split -b ${CHUNK_MB}M -d -a 4 "$WORK/data" "$WORK/in/c"
N=$(ls "$WORK/in" | wc -l)
ref_sha="$(sha256sum < "$WORK/data" | cut -d' ' -f1)"

# one work queue for all workers
next_job() {
    ( flock 9; n=$(cat "$WORK/next"); [ "$n" -lt "$N" ] || exit 1; echo $((n + 1)) > "$WORK/next"; echo "$n" ) 9>"$WORK/lock"
}
worker() {   # $1 = local|remote, $2 = out dir
    local j f
    while j=$(next_job); do
        f=$(printf 'c%04d' "$j")
        if [ "$1" = local ]; then
            xz -6 -T1 -c < "$WORK/in/$f" > "$2/$f.xz"
        else
            c2crun "xz -6 -T1 -c" < "$WORK/in/$f" > "$2/$f.xz"
        fi
        echo "$1" > "$2/$f.who"
    done
}
run_pool() {   # $1 = out dir, $2 = local workers, $3 = remote workers
    echo 0 > "$WORK/next"
    local i
    for ((i = 0; i < $2; i++)); do worker local "$1" & done
    for ((i = 0; i < $3; i++)); do worker remote "$1" & done
    wait
}

say "A) master only: $N chunks x ${CHUNK_MB} MiB, 8 workers"
t0=$(now); run_pool "$WORK/a" 8 0; t1=$(now)
ta=$(python3 -c "print(round($t1 - $t0, 2))")
say "   ${ta} s"

say "B) pooled: 8 workers on master + 8 on slave (over c2c)"
rx0=$(link_json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["tx_bytes"], d["rx_bytes"])')
( sleep 6; echo "$(local_field cpu_util) $(peer_field cpu_util)" > "$WORK/util_mid" ) &
t0=$(now); run_pool "$WORK/b" 8 8; t1=$(now)
tb=$(python3 -c "print(round($t1 - $t0, 2))")
rx1=$(link_json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["tx_bytes"], d["rx_bytes"])')
wait
say "   ${tb} s"

say "verifying pooled output"
out_sha="$(for f in $(ls "$WORK/in"); do xz -dc "$WORK/b/$f.xz"; done | sha256sum | cut -d' ' -f1)"
n_remote=$(grep -l remote "$WORK"/b/*.who | wc -l)
read -r cpu_m cpu_s < "$WORK/util_mid" 2>/dev/null || { cpu_m=0; cpu_s=0; }
comp=$(cat "$WORK"/b/*.xz | wc -c)

metrics="$(python3 - <<EOF
import json
a, b = $ta, $tb
t0, r0 = map(int, "$rx0".split()); t1, r1 = map(int, "$rx1".split())
print(json.dumps({"data_mib": $DATA_MB, "chunks": $N, "chunk_mib": $CHUNK_MB,
  "master_only_s": a, "pooled_s": b, "speedup": round(a / b, 2),
  "master_only_mib_s": round($DATA_MB / a, 1), "pooled_mib_s": round($DATA_MB / b, 1),
  "chunks_done_by_slave": $n_remote, "compressed_mib": round($comp / 2**20, 1),
  "link_tx_mib": round((t1 - t0) / 2**20, 1), "link_rx_mib": round((r1 - r0) / 2**20, 1),
  "cpu_util_mid_run": {"master": $cpu_m, "slave": $cpu_s},
  "verified": "$out_sha" == "$ref_sha"}))
EOF
)"
speedup=$(python3 -c "print(round($ta / $tb, 2))")
summary="xz ${DATA_MB} MiB: master-only ${ta} s, pooled 16 cores ${tb} s = ${speedup}x; slave did ${n_remote}/${N} chunks; CPU mid-run master ${cpu_m}% slave ${cpu_s}%"
if [ "$out_sha" = "$ref_sha" ] && python3 -c "import sys; sys.exit(0 if $speedup >= 1.6 else 1)"; then
    result PASS "$summary" "$metrics"
else
    [ "$out_sha" = "$ref_sha" ] || summary="$summary; OUTPUT MISMATCH"
    result FAIL "$summary" "$metrics"
fi
