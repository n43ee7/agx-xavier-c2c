# test workloads - plain programs, no C2C code in them. built on both boards
OUT  ?= ../build
NVCC ?= /usr/local/cuda/bin/nvcc

all: $(OUT)/mem_proof $(OUT)/gpu_probe $(OUT)/gpu_check

$(OUT)/mem_proof: mem_proof.c
	@mkdir -p $(OUT)
	$(CC) -O2 -Wall -o $@ $<

$(OUT)/gpu_probe: gpu_probe.cu
	@mkdir -p $(OUT)
	$(NVCC) -O2 -arch=sm_72 -o $@ $< -lcublas

$(OUT)/gpu_check: gpu_check.cu
	@mkdir -p $(OUT)
	$(NVCC) -O2 -arch=sm_72 -o $@ $< -lcublas

clean:
	rm -f $(OUT)/mem_proof $(OUT)/gpu_probe $(OUT)/gpu_check

.PHONY: all clean
