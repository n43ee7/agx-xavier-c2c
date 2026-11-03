// fatdump FILE [sm]: dumps the param layout of every kernel in every fatbin
// inside FILE. Only used to check fatbin.cpp against cuobjdump -elf.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "fatbin.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: fatdump FILE [sm]\n"); return 2; }
    int sm = argc > 2 ? std::atoi(argv[2]) : 72;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    size_t nfat = 0, nk = 0, nerr = 0;
    for (size_t i = 0; i + 16 <= d.size(); i += 8) {
        uint32_t m; uint16_t ver, hs; uint64_t ps;
        std::memcpy(&m, &d[i], 4);
        if (m != c2cgpu::kFatbinMagic) continue;
        std::memcpy(&ver, &d[i + 4], 2); std::memcpy(&hs, &d[i + 6], 2); std::memcpy(&ps, &d[i + 8], 8);
        if (ver != 1 || hs != 16 || i + hs + ps > d.size()) continue;
        nfat++;
        auto R = c2cgpu::parse_image(&d[i], sm);
        if (!R.error.empty()) { nerr++; std::printf("# fatbin@%zx error: %s\n", i, R.error.c_str()); }
        for (auto& k : R.kernels) {
            nk++;
            std::printf("%s total=%u", k.first.c_str(), k.second.total);
            for (auto& p : k.second.params) std::printf(" %u:%u", p.first, p.second);
            std::printf("\n");
        }
        i += hs + ps - 8;
    }
    std::fprintf(stderr, "fatbins %zu kernels %zu errors %zu\n", nfat, nk, nerr);
    return nerr ? 1 : 0;
}
