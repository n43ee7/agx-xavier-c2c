#include "fatbin.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace c2cgpu {

namespace {

#pragma pack(push, 1)
struct FatHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t payload_size;  // entry bytes following this header
};
struct FatEntry {
    uint16_t kind;          // 1 PTX, 2 ELF
    uint16_t unknown0;
    uint32_t header_size;
    uint64_t payload_size;  // payload bytes after the entry hdr (padded)
    uint32_t compressed_size;
    uint32_t unknown1;
    uint16_t minor, major;
    uint32_t arch;          // e.g. 72
    uint32_t name_offset, name_len;
    uint64_t flags;         // 0x2000 = compressed
    uint64_t zero;
    uint64_t decompressed_size;
};
struct Elf64Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf64Shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
#pragma pack(pop)

constexpr uint64_t kCompressed = 0x2000;

bool is_elf(const uint8_t* p) { return p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F'; }

size_t elf_size(const uint8_t* p) {
    auto* h = reinterpret_cast<const Elf64Ehdr*>(p);
    size_t end = h->shoff + size_t(h->shnum) * h->shentsize;
    end = std::max<size_t>(end, h->phoff + size_t(h->phnum) * h->phentsize);
    auto* sh = reinterpret_cast<const Elf64Shdr*>(p + h->shoff);
    for (int i = 0; i < h->shnum; i++)
        if (sh[i].type != 8 /*NOBITS*/) end = std::max<size_t>(end, sh[i].offset + sh[i].size);
    return end;
}

// one entry's payload, decompressed if it has to be
bool entry_payload(const FatEntry* e, std::vector<uint8_t>& out) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(e) + e->header_size;
    if (e->flags & kCompressed) {
        out.resize(e->decompressed_size);
        size_t n = fatbin_decompress(p, e->compressed_size, out.data(), out.size());
        if (n != e->decompressed_size) return false;
    } else {
        out.assign(p, p + e->payload_size);
    }
    return true;
}

}  // namespace

size_t fatbin_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
    size_t ip = 0, op = 0;
    while (ip < in_len) {
        uint8_t tok = in[ip++];
        size_t lit = tok >> 4, mlen = (tok & 0xf) + 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= in_len) return op;
                b = in[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > in_len || op + lit > out_len) return op;
        std::memcpy(out + op, in + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= in_len || op >= out_len) break;  // last sequence = literals only
        if (ip + 2 > in_len) return op;
        size_t back = in[ip] | (size_t(in[ip + 1]) << 8);
        ip += 2;
        if (mlen == 15 + 4) {
            uint8_t b;
            do {
                if (ip >= in_len) return op;
                b = in[ip++];
                mlen += b;
            } while (b == 255);
        }
        if (back == 0 || back > op || op + mlen > out_len) return op;
        for (size_t i = 0; i < mlen; i++) out[op + i] = out[op - back + i];  // can overlap, so no memcpy
        op += mlen;
    }
    return op;
}

size_t image_size(const void* image) {
    auto* p = static_cast<const uint8_t*>(image);
    uint32_t magic;
    std::memcpy(&magic, p, 4);
    if (magic == kFatbinWrapperMagic) {  // __fatBinC_Wrapper_t {magic, version, data, ...}
        const void* data;
        std::memcpy(&data, p + 8, sizeof(data));
        return image_size(data);
    }
    if (magic == kFatbinMagic) {
        auto* h = reinterpret_cast<const FatHeader*>(p);
        return h->header_size + h->payload_size;
    }
    if (is_elf(p)) return elf_size(p);
    return std::strlen(reinterpret_cast<const char*>(p)) + 1;  // PTX text
}

bool parse_elf_params(const uint8_t* elf, size_t n, std::map<std::string, ParamLayout>& out,
                      std::string& err) {
    if (n < sizeof(Elf64Ehdr) || !is_elf(elf) || elf[4] != 2) {
        err = "not a 64-bit ELF";
        return false;
    }
    auto* h = reinterpret_cast<const Elf64Ehdr*>(elf);
    if (h->shoff + size_t(h->shnum) * sizeof(Elf64Shdr) > n || h->shstrndx >= h->shnum) {
        err = "bad section table";
        return false;
    }
    auto* sh = reinterpret_cast<const Elf64Shdr*>(elf + h->shoff);
    const char* shstr = reinterpret_cast<const char*>(elf + sh[h->shstrndx].offset);
    static const char kPrefix[] = ".nv.info.";
    for (int i = 0; i < h->shnum; i++) {
        const char* name = shstr + sh[i].name;
        if (std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) continue;
        if (sh[i].offset + sh[i].size > n) continue;
        std::string kernel = name + sizeof(kPrefix) - 1;
        ParamLayout L;
        bool have_total = false;
        std::map<uint16_t, std::pair<uint32_t, uint32_t>> by_ord;
        const uint8_t* a = elf + sh[i].offset;
        const uint8_t* e = a + sh[i].size;
        while (a + 2 <= e) {
            uint8_t fmt = a[0], attr = a[1];
            a += 2;
            size_t vlen = 0;
            const uint8_t* v = a;
            if (fmt == 1) vlen = 0;
            else if (fmt == 2) vlen = 1;
            else if (fmt == 3) vlen = 2;
            else if (fmt == 4) {
                if (a + 2 > e) break;
                vlen = a[0] | (a[1] << 8);
                v = a + 2;
                a += 2;
            } else break;
            if (v + vlen > e) break;
            if (fmt == 4 && attr == 0x17 && vlen >= 12) {  // EIATTR_KPARAM_INFO
                uint16_t ord, off;
                uint32_t bits;
                std::memcpy(&ord, v + 4, 2);
                std::memcpy(&off, v + 6, 2);
                std::memcpy(&bits, v + 8, 4);
                by_ord[ord] = {off, (bits >> 18) & 0x3fff};
            } else if (fmt == 3 && attr == 0x19) {  // EIATTR_CBANK_PARAM_SIZE
                L.total = v[0] | (v[1] << 8);
                have_total = true;
            } else if (fmt == 4 && attr == 0x0a && vlen >= 8 && !have_total) {  // PARAM_CBANK
                uint16_t sz;
                std::memcpy(&sz, v + 6, 2);
                L.total = sz;
            }
            a = v + vlen;
        }
        for (auto& kv : by_ord) {
            L.params.push_back(kv.second);
            L.total = std::max(L.total, kv.second.first + kv.second.second);
        }
        out[kernel] = std::move(L);
    }
    return true;
}

bool parse_ptx_params(const char* ptx, size_t n, std::map<std::string, ParamLayout>& out) {
    std::string s(ptx, strnlen(ptx, n));
    size_t pos = 0;
    while ((pos = s.find(".entry", pos)) != std::string::npos) {
        pos += 6;
        size_t lp = s.find('(', pos), brace = s.find('{', pos);
        if (lp == std::string::npos) break;
        std::string name;
        for (size_t i = pos; i < std::min(lp, brace); i++)
            if (!std::isspace(uint8_t(s[i]))) name += s[i];
        ParamLayout L;
        if (lp < brace) {
            size_t rp = s.find(')', lp);
            std::string plist = s.substr(lp + 1, rp - lp - 1);
            uint32_t off = 0;
            size_t q = 0;
            while ((q = plist.find(".param", q)) != std::string::npos) {
                size_t end = plist.find(',', q);
                std::string decl = plist.substr(q, end == std::string::npos ? std::string::npos : end - q);
                q = end == std::string::npos ? plist.size() : end + 1;
                uint32_t esz = 0, align = 0, count = 1;
                static const std::pair<const char*, uint32_t> kTypes[] = {
                    {".b8", 1}, {".u8", 1}, {".s8", 1}, {".b16", 2}, {".u16", 2}, {".s16", 2},
                    {".f16", 2}, {".b32", 4}, {".u32", 4}, {".s32", 4}, {".f32", 4}, {".b64", 8},
                    {".u64", 8}, {".s64", 8}, {".f64", 8}};
                for (auto& t : kTypes) {
                    size_t tp = decl.find(t.first);
                    if (tp != std::string::npos && !std::isalnum(uint8_t(decl[tp + std::strlen(t.first)]))) {
                        esz = t.second;
                        break;
                    }
                }
                size_t ap = decl.find(".align");
                if (ap != std::string::npos) align = uint32_t(std::strtoul(decl.c_str() + ap + 6, nullptr, 10));
                size_t bp = decl.find('[');
                if (bp != std::string::npos) count = uint32_t(std::strtoul(decl.c_str() + bp + 1, nullptr, 10));
                if (!esz) esz = 1;
                if (!align) align = esz;
                off = (off + align - 1) / align * align;
                L.params.push_back({off, esz * count});
                off += esz * count;
            }
            L.total = off;
        }
        out[name] = std::move(L);
        pos = lp;
    }
    return true;
}

ParsedImage parse_image(const void* image, int sm) {
    ParsedImage R;
    auto* p = static_cast<const uint8_t*>(image);
    uint32_t magic;
    std::memcpy(&magic, p, 4);
    if (magic == kFatbinWrapperMagic) {
        const void* data;
        std::memcpy(&data, p + 8, sizeof(data));
        return parse_image(data, sm);
    }
    if (is_elf(p)) {
        size_t n = elf_size(p);
        R.load_image.assign(p, p + n);
        parse_elf_params(p, n, R.kernels, R.error);
        return R;
    }
    if (magic != kFatbinMagic) {  // PTX text
        size_t n = std::strlen(reinterpret_cast<const char*>(p)) + 1;
        R.load_image.assign(p, p + n);
        parse_ptx_params(reinterpret_cast<const char*>(p), n, R.kernels);
        return R;
    }
    auto* h = reinterpret_cast<const FatHeader*>(p);
    const uint8_t* q = p + h->header_size;
    const uint8_t* end = q + h->payload_size;
    // pick what the driver itself would pick: ELF for the exact arch, then the
    // newest same-major ELF <= device, then the newest PTX <= device
    const FatEntry *best_elf = nullptr, *best_ptx = nullptr;
    while (q + sizeof(FatEntry) <= end) {
        auto* e = reinterpret_cast<const FatEntry*>(q);
        if (e->header_size == 0) break;
        int arch = int(e->arch);
        if (e->kind == 2 && arch <= sm && arch / 10 == sm / 10 &&
            (!best_elf || int(best_elf->arch) < arch))
            best_elf = e;
        if (e->kind == 1 && arch <= sm && (!best_ptx || int(best_ptx->arch) < arch)) best_ptx = e;
        q += e->header_size + e->payload_size;
    }
    const FatEntry* pick = best_elf ? best_elf : best_ptx;
    if (!pick) {
        R.error = "no image for sm_" + std::to_string(sm);
        return R;
    }
    // what we ship to the slave: a fatbin w/ just that one entry, left compressed
    size_t elen = pick->header_size + pick->payload_size;
    FatHeader nh{kFatbinMagic, 1, uint16_t(sizeof(FatHeader)), elen};
    R.load_image.resize(sizeof nh + elen);
    std::memcpy(R.load_image.data(), &nh, sizeof nh);
    std::memcpy(R.load_image.data() + sizeof nh, pick, elen);
    std::vector<uint8_t> body;
    if (!entry_payload(pick, body)) {
        R.error = "decompression failed";
        return R;
    }
    if (pick->kind == 2) parse_elf_params(body.data(), body.size(), R.kernels, R.error);
    else parse_ptx_params(reinterpret_cast<const char*>(body.data()), body.size(), R.kernels);
    return R;
}

}  // namespace c2cgpu
