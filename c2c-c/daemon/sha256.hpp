// small SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104), no deps
#pragma once
#include <cstddef>
#include <cstdint>

namespace c2c {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void final(uint8_t out[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t used_ = 0;
};

// HMAC-SHA256 over up to three parts glued together
void hmac_sha256(const uint8_t* key, size_t keylen,
                 const void* a, size_t alen,
                 const void* b, size_t blen,
                 const void* c, size_t clen,
                 uint8_t out[32]);

// constant time compare
bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n);

}  // namespace c2c
