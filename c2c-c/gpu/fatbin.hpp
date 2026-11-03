// fatbin.hpp: the bare minimum of NVIDIA's fatbin / cubin (ELF) / PTX formats.
// Two jobs: trim a fatbin down to the one image the remote GPU loads, and dig
// out each kernel's param layout so we can serialise cuLaunchKernel's
// void** kernelParams.
//
// Fatbin (CUDA 11): header {u32 magic 0xBA55ED50, u16 ver 1, u16 hdr size 16,
// u64 payload size}, then entries = header (kind 1 PTX / 2 ELF, sm arch,
// flags, sizes) + payload. flag 0x2000 means the payload is LZ4-ish compressed.
// Cubin: each kernel has a ".nv.info.<kernel>" section. Param offsets/sizes
// come from EIATTR_KPARAM_INFO (ordinal, offset, size), the total size from
// EIATTR_CBANK_PARAM_SIZE / EIATTR_PARAM_CBANK.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace c2cgpu {

struct ParamLayout {
    uint32_t total = 0;                                 // param buffer size in bytes
    std::vector<std::pair<uint32_t, uint32_t>> params;  // (offset, size) by ordinal
};

struct ParsedImage {
    std::vector<uint8_t> load_image;               // goes to cuModuleLoadData on the slave
    std::map<std::string, ParamLayout> kernels;    // mangled name -> layout
    std::string error;                             // set if parsing failed
};

static constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
static constexpr uint32_t kFatbinWrapperMagic = 0x466243b1u;

// size of a cuModuleLoadData image (fatbin/ELF/PTX), 0 = don't know
size_t image_size(const void* image);
// undo NVIDIA's fatbin payload compression, returns bytes written
size_t fatbin_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
// parse a module image for an sm_XX device (72 for Xavier)
ParsedImage parse_image(const void* image, int sm);
// same, but for a bare cubin or PTX (no fatbin wrapper)
bool parse_elf_params(const uint8_t* elf, size_t n, std::map<std::string, ParamLayout>& out,
                      std::string& err);
bool parse_ptx_params(const char* ptx, size_t n, std::map<std::string, ParamLayout>& out);

}  // namespace c2cgpu
