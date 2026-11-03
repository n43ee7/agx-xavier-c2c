#!/bin/bash
# Builds NVIDIA's interconnect benchmarks on the MASTER, into ~/c2c-bench.
# Run it as your normal user, it only sudos once for apt.
#   bench/nvidia/build.sh
# Versions pinned to what actually works w/ CUDA 11.4 / JetPack 5:
#   NCCL v2.18.6-1, nccl-tests v2.13.9, nvbandwidth v0.5 (+ patch), NVML stub.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HOME/c2c-bench
export PATH=/usr/local/cuda/bin:$HOME/.local/bin:$PATH
GEN="-gencode=arch=compute_72,code=sm_72"
mkdir -p "$W" && cd "$W"

sudo apt-get install -y -qq libboost-program-options-dev python3-pip
python3 -m pip install --user -q "cmake>=3.20,<3.30"   # nvbandwidth needs CMake >= 3.20 (20.04 has 3.16)

for r in nvbandwidth nccl nccl-tests; do [ -d $r ] || git clone -q https://github.com/NVIDIA/$r; done

# no NVML (libnvidia-ml) on Jetson, but nvbandwidth links it and NCCL dlopens it
mkdir -p nvml-stub
gcc -O2 -shared -fPIC -Wl,-soname,libnvidia-ml.so.1 -o nvml-stub/libnvidia-ml.so.1 "$HERE/nvml_stub.c" -ldl

(cd nccl && git checkout -q -f v2.18.6-1 && make -j8 src.build NVCC_GENCODE="$GEN")
(cd nccl-tests && git checkout -q -f v2.13.9 && make clean >/dev/null && \
    make -j8 MPI=0 CUDA_HOME=/usr/local/cuda NCCL_HOME="$W/nccl/build" NVCC_GENCODE="$GEN")
# nvbandwidth v0.6+ wants CUDA 12 headers. The patch skips the host-memory
# start latch on GPUs that can't map host memory (= the remote GPU).
(cd nvbandwidth && git checkout -q -f v0.5 && git apply "$HERE/nvbandwidth-v0.5-no-host-latch.patch" && \
    rm -rf build && cmake -S . -B build -DCMAKE_CUDA_ARCHITECTURES=72 && cmake --build build -j8)

echo "built: $W/nvbandwidth/build/nvbandwidth, $W/nccl-tests/build/*_perf"
echo "run with: export LD_LIBRARY_PATH=$W/nccl/build/lib:$W/nvml-stub"
