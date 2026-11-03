# Remote RAM helpers for T04/T05 (source after common.sh).
# Starts c2c-memd on the slave, attaches it on the master as prio-100 swap
# (ahead of L4T's zram at prio 5), samples usage, and the trap always undoes
# everything, pass or fail.

RMEM_DEV=/dev/nbd0
RMEM_STATS=/tmp/c2c-memd.json
RMEM_SAMPLES=""
RMEM_SAMPLER=""
# swap readahead = 2^N pages per fault. c2c-rmem does the kernel tuning and
# restores it itself (page-cluster, vma_ra_enabled, min_free_kbytes, watermarks)
RMEM_PAGE_CLUSTER="${C2C_RMEM_PAGE_CLUSTER:-7}"
RMEM_CONNS="${C2C_RMEM_CONNS:-2}"
RMEM_EXTRA="${C2C_RMEM_ARGS:-}"      # extra c2c-rmem attach flags, for experiments

rmem_cleanup() {
    [ -n "$RMEM_SAMPLER" ] && kill "$RMEM_SAMPLER" 2>/dev/null
    if c2c-rmem status -d "$RMEM_DEV" >/dev/null 2>&1; then
        say "detaching remote memory"
        c2c-rmem detach -d "$RMEM_DEV" >&2
    fi
    c2crun "pkill -x c2c-memd" 2>/dev/null
}

rmem_start() {   # $1 = how much to lend, e.g. 16G
    trap rmem_cleanup EXIT
    c2crun "pkill -x c2c-memd; sleep 0.3; nohup /opt/c2c/bin/c2c-memd -s $1 -S $RMEM_STATS >/tmp/c2c-memd.log 2>&1 </dev/null &"
    sleep 1
    c2c-rmem attach -d "$RMEM_DEV" -c "$RMEM_CONNS" -w 100 -r "$RMEM_PAGE_CLUSTER" $RMEM_EXTRA >&2 || { warn "attach failed"; return 1; }
    RMEM_SAMPLES="$RESULTS/$TEST_ID.samples"
    : > "$RMEM_SAMPLES"
    (   # t, remote swap used (KiB), zram used (KiB), slave memd resident (bytes)
        t0=$(date +%s.%N); i=0
        while :; do
            nb=$(awk -v d="$RMEM_DEV" '$1==d {print $4}' /proc/swaps)
            zr=$(awk '$1 ~ /zram/ {s+=$4} END {print s+0}' /proc/swaps)
            if (( i % 2 == 0 )); then
                res=$(c2crun "cat $RMEM_STATS" 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["resident"])' 2>/dev/null || echo 0)
            fi
            echo "$(python3 -c "print(round($(date +%s.%N)-$t0,1))") ${nb:-0} ${zr:-0} ${res:-0}" >> "$RMEM_SAMPLES"
            i=$((i + 1)); sleep 1
        done
    ) &
    RMEM_SAMPLER=$!
}

rmem_stop_sampler() { kill "$RMEM_SAMPLER" 2>/dev/null; wait "$RMEM_SAMPLER" 2>/dev/null; RMEM_SAMPLER=""; }

# peaks from the samples: "peak_remote_kib peak_zram_kib peak_memd_bytes"
rmem_peaks() { awk '{if($2>a)a=$2; if($3>b)b=$3; if($4>c)c=$4} END {printf "%.0f %.0f %.0f\n", a, b, c}' "$RMEM_SAMPLES"; }
