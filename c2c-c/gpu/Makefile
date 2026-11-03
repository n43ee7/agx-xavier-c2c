# C2C remote GPU
#   libc2ccuda.so  master: fake libcuda.so.1 that gets loaded into normal CUDA programs
#   c2c-gpud       slave:  serves its GPU to the master
#   c2c-gpu-ls     list the GPUs a program sees (local + remote)
#   c2c-gpu-bench  remote GPU bandwidth / duplex / latency
#   fatdump        sanity checker for the kernel-param parser
CUDA        ?= /usr/local/cuda
REAL_LIBCUDA ?= $(firstword $(wildcard /usr/lib/aarch64-linux-gnu/tegra/libcuda.so.1.1 \
                                       /usr/lib/aarch64-linux-gnu/libcuda.so.1 /usr/lib/x86_64-linux-gnu/libcuda.so.1))
CUDA_LIBDIR ?= $(dir $(REAL_LIBCUDA))
CXX         ?= g++
CXXFLAGS    += -O2 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -I$(CUDA)/include -pthread

all: libc2ccuda.so c2c-gpud c2c-gpu-ls c2c-gpu-bench fatdump

build/syms.txt: $(REAL_LIBCUDA)
	mkdir -p build
	nm -D --defined-only $(REAL_LIBCUDA) | awk '$$2=="T" {print $$3}' | sort -u > $@

build/tramp.S build/tramp_names.inc: build/syms.txt libc2ccuda.cpp gen_tramp.py
	python3 gen_tramp.py build/syms.txt libc2ccuda.cpp build

libc2ccuda.so: libc2ccuda.cpp fatbin.cpp fatbin.hpp gpu_proto.hpp build/tramp.S build/tramp_names.inc
	$(CXX) $(CXXFLAGS) -fPIC -shared -fvisibility=hidden -Ibuild -o $@ libc2ccuda.cpp fatbin.cpp \
	    build/tramp.S -Wl,-soname,libcuda.so.1 -Wl,--no-undefined -ldl
	mkdir -p lib/cuda && ln -sf ../../libc2ccuda.so lib/cuda/libcuda.so.1

c2c-gpud: c2c-gpud.cpp gpu_proto.hpp
	$(CXX) $(CXXFLAGS) -o $@ c2c-gpud.cpp -L$(CUDA_LIBDIR) -lcuda

c2c-gpu-ls: c2c-gpu-ls.cpp
	$(CXX) $(CXXFLAGS) -o $@ c2c-gpu-ls.cpp -L$(CUDA_LIBDIR) -lcuda

c2c-gpu-bench: c2c-gpu-bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ c2c-gpu-bench.cpp -L$(CUDA_LIBDIR) -lcuda

fatdump: fatdump.cpp fatbin.cpp fatbin.hpp
	$(CXX) $(CXXFLAGS) -o $@ fatdump.cpp fatbin.cpp

clean:
	rm -rf build lib libc2ccuda.so c2c-gpud c2c-gpu-ls c2c-gpu-bench fatdump

.PHONY: all clean
