// Wire format that rides inside the USB STREAM byte stream.
//
//   frame = FrameHdr (12 bytes, little endian) + payload[len]
//
// channels:
//   CH_CTRL  handshake, heartbeat, telemetry, all small stuff
//   CH_NET   one IP packet with a 10 byte virtio_net_hdr in front so the
//            csum offload state makes it across. USB already CRCs every
//            packet so we never bother computing TCP/UDP checksums here.
//
// Parser resyncs on magic + header checksum, so after a reconnect or a
// half frame the stream sorts itself out quickly.
#pragma once
#include <cstddef>
#include <cstdint>

namespace c2c {

constexpr uint32_t kMagic = 0x31433243;  // "C2C1"
constexpr uint16_t kProtoVersion = 1;
constexpr uint32_t kMaxPayload = 128 * 1024;
constexpr size_t kNonceLen = 16;
constexpr size_t kMacLen = 32;

enum Chan : uint8_t { CH_CTRL = 0, CH_NET = 1 };

enum Ctrl : uint8_t {
    HELLO_REQ = 1,  // slave -> master: "I just (re)started, do the handshake"
    HELLO = 2,      // master -> slave: version, nonce_m
    CHALLENGE = 3,  // slave -> master: nonce_s, HMAC(key,"S"|nm|ns)
    AUTH = 4,       // master -> slave: HMAC(key,"M"|ns|nm)
    READY = 5,      // slave -> master: HMAC(key,"R"|nm|ns)  => link UP
    REJECT = 6,     // either: reason
    PING = 7,
    PONG = 8,
    INFO = 9,       // SysInfo telemetry
    BYE = 10,       // orderly shutdown
};

enum RejectReason : uint32_t { REJ_BAD_KEY = 1, REJ_VERSION = 2, REJ_ROLE = 3 };

enum Role : uint8_t { ROLE_MASTER = 1, ROLE_SLAVE = 2 };

#pragma pack(push, 1)
struct FrameHdr {
    uint32_t magic;
    uint32_t len;   // payload bytes
    uint8_t chan;
    uint8_t type;
    uint16_t hsum;  // checksum over the 10 bytes above
};
static_assert(sizeof(FrameHdr) == 12, "FrameHdr layout");

struct MsgHello {
    uint16_t version;
    uint8_t role;
    uint8_t rsv;
    uint8_t nonce[kNonceLen];
    char name[32];
};

struct MsgChallenge {
    uint16_t version;
    uint8_t role;
    uint8_t rsv;
    uint8_t nonce[kNonceLen];
    uint8_t mac[kMacLen];
    char name[32];
};

struct MsgMac {
    uint8_t mac[kMacLen];
};

struct MsgReject {
    uint32_t reason;
};

struct MsgPing {
    uint64_t seq;
    uint64_t t_ns;
};

// telemetry each node sends every 2 s
struct SysInfo {
    uint32_t version;
    char host[32];
    char l4t[24];
    char cuda[16];
    uint16_t ncpu;
    uint16_t ncpu_online;
    float cpu_util;        // %
    float load1;
    uint64_t mem_total_kb;
    uint64_t mem_avail_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    float gpu_util;        // %
    uint32_t gpu_mhz;
    float temp_cpu;        // C
    float temp_gpu;        // C
    uint8_t nvenc, nvdec, nvjpg, dla;
    uint32_t uptime_s;
};
#pragma pack(pop)

inline uint16_t hdr_sum(const FrameHdr& h) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&h);
    uint32_t s = 0x9E37u;
    for (int i = 0; i < 10; i++) s = (s * 31u) ^ p[i];
    return static_cast<uint16_t>(s ^ (s >> 16));
}

inline FrameHdr make_hdr(uint8_t chan, uint8_t type, uint32_t len) {
    FrameHdr h{kMagic, len, chan, type, 0};
    h.hsum = hdr_sum(h);
    return h;
}

}  // namespace c2c
