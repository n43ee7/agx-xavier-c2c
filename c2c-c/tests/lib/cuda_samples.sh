# Stock NVIDIA CUDA samples (setup_tests.sh builds them into $BUILD/samples),
# run via c2c-gpu in one of 3 modes:
#   local   C2C_GPU_DISABLE=1 -> only our own GPU, still through the shim (regression check)
#   remote  CUDA_VISIBLE_DEVICES=1 -> only the peer's GPU, shows up as device 0
#   all     both GPUs (the multi-GPU samples use both)
# One line per sample: "<status> <seconds> <name>". status = PASS, FAIL or
# WAIVED (sample exited 2 = feature not there on that GPU).

SAMPLES_DIR="$BUILD/samples"
# name|args  (the dir under $SAMPLES_DIR gets found by name)
SAMPLES_LIST="deviceQuery deviceQueryDrv bandwidthTest|--mode=quick vectorAdd vectorAddDrv matrixMul matrixMulDrv
simpleMultiGPU simpleStreams asyncAPI simpleAtomicIntrinsics clock simpleTemplates simpleCallback
simpleOccupancy cppIntegration simpleVoteIntrinsics simpleMultiCopy simpleZeroCopy
simpleCooperativeGroups UnifiedMemoryStreams simpleTexture inlinePTX simpleAWBarrier reduction scan transpose
threadFenceReduction mergeSort sortingNetworks fastWalshTransform shfl_scan concurrentKernels BlackScholes
binomialOptions MonteCarloMultiGPU|-method=threaded quasirandomGenerator SobolQRNG convolutionSeparable histogram
dct8x8 dwtHaar1D simpleCUBLAS batchCUBLAS simpleCUFFT conjugateGradient MersenneTwisterGP11213 simpleCUBLASXT
p2pBandwidthLatencyTest"

sample_bin() {   # $1 = name -> path to its binary
    find "$SAMPLES_DIR" -maxdepth 3 -type f -name "$1" -perm -u+x 2>/dev/null | head -1
}

run_sample() {   # $1 = mode, $2 = name, $3.. = args ; prints the status line, log goes to $SAMPLE_LOG_DIR
    local mode=$1 name=$2; shift 2
    local bin; bin="$(sample_bin "$name")"
    [ -n "$bin" ] || { echo "MISSING 0 $name"; return; }
    local log="$SAMPLE_LOG_DIR/$mode.$name.log" t0 t1 rc
    t0=$(date +%s.%N)
    (
        cd "$(dirname "$bin")"
        case $mode in
            local)  C2C_GPU_DISABLE=1 timeout 300 c2c-gpu run "$bin" "$@" ;;
            remote) CUDA_VISIBLE_DEVICES=1 timeout 300 c2c-gpu run "$bin" "$@" ;;
            all)    timeout 300 c2c-gpu run "$bin" "$@" ;;
        esac
    ) </dev/null >"$log" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    local st=FAIL
    [ $rc = 0 ] && st=PASS
    [ $rc = 2 ] && st=WAIVED
    # some samples print a failure but still exit 0
    grep -qiE "Result = FAIL|Test failed|FAILED" "$log" && st=FAIL
    printf "%s %.2f %s\n" "$st" "$(echo "$t1 - $t0" | bc)" "$name"
}

run_samples() {  # $1 = mode
    local entry name args
    for entry in $SAMPLES_LIST; do
        name="${entry%%|*}"; args=""
        [ "$entry" != "$name" ] && args="${entry#*|}"
        # shellcheck disable=SC2086
        run_sample "$1" "$name" $args
    done
}
