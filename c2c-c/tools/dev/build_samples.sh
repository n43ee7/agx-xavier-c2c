#!/bin/bash
# builds a big chunk of the stock NVIDIA CUDA samples for sm_72, untouched
set -u
SRC=/usr/local/cuda/samples
DST=${1:-$HOME/C2C-thunderbolt/tests/build/samples}
mkdir -p "$DST"
rsync -a --delete --exclude '*.o' "$SRC/" "$DST/" 2>/dev/null || cp -a "$SRC/." "$DST/"
LIST="1_Utilities/deviceQuery 1_Utilities/deviceQueryDrv 1_Utilities/bandwidthTest 1_Utilities/p2pBandwidthLatencyTest
0_Simple/vectorAdd 0_Simple/vectorAddDrv 0_Simple/matrixMul 0_Simple/matrixMulDrv 0_Simple/simpleMultiGPU
0_Simple/simpleStreams 0_Simple/asyncAPI 0_Simple/simpleAtomicIntrinsics 0_Simple/clock 0_Simple/simpleTemplates
0_Simple/simpleCallback 0_Simple/simpleOccupancy 0_Simple/cppIntegration 0_Simple/simpleVoteIntrinsics
0_Simple/simpleMultiCopy 0_Simple/simpleHyperQ 0_Simple/simpleZeroCopy 0_Simple/simpleP2P 0_Simple/simpleCooperativeGroups
0_Simple/UnifiedMemoryStreams 0_Simple/simpleTexture 0_Simple/simplePrintf 0_Simple/inlinePTX 0_Simple/simpleAWBarrier
6_Advanced/reduction 6_Advanced/scan 6_Advanced/transpose 6_Advanced/threadFenceReduction 6_Advanced/mergeSort
6_Advanced/sortingNetworks 6_Advanced/fastWalshTransform 6_Advanced/shfl_scan 6_Advanced/concurrentKernels
4_Finance/BlackScholes 4_Finance/binomialOptions 4_Finance/MonteCarloMultiGPU 4_Finance/quasirandomGenerator 4_Finance/SobolQRNG
3_Imaging/convolutionSeparable 3_Imaging/histogram 3_Imaging/dct8x8 3_Imaging/dwtHaar1D
7_CUDALibraries/matrixMulCUBLAS 7_CUDALibraries/simpleCUBLAS 7_CUDALibraries/batchCUBLAS 7_CUDALibraries/simpleCUFFT
7_CUDALibraries/conjugateGradient 7_CUDALibraries/MersenneTwisterGP11213 7_CUDALibraries/simpleCUBLASXT"
ok=0; bad=0
for s in $LIST; do
    [ -d "$DST/$s" ] || { echo "MISSING $s"; continue; }
    if make -C "$DST/$s" SMS=72 -j8 >"$DST/$s/build.log" 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "BUILD-FAIL $s"; fi
done
echo "built $ok, failed $bad"
