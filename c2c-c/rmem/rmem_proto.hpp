// rmem: slave lends RAM to the master as a block device.
// TCP over c2c0. We do a tiny handshake of our own, then the socket goes to
// the kernel's NBD client which talks plain NBD from there on. Hook it up as
// high-priority swap and normal programs on the master end up using the
// slave's RAM.
#pragma once
#include <arpa/inet.h>
#include <endian.h>
#include <stdint.h>

namespace rmem {

constexpr uint16_t kDefaultPort = 10809;
constexpr char kHelloClient[8] = {'C', '2', 'C', 'R', 'M', 'E', 'M', '1'};
constexpr char kHelloServer[8] = {'C', '2', 'C', 'R', 'M', 'S', 'V', '1'};

#pragma pack(push, 1)
struct ServerHello {          // server -> client, right after kHelloClient
    char magic[8];            // kHelloServer
    uint64_t size_be;         // export size in bytes
    uint32_t flags_be;        // reserved
};
// plain NBD transmission phase, big endian
struct NbdRequest {
    uint32_t magic;           // 0x25609513
    uint32_t type;            // low 16 = command, high 16 = flags
    uint64_t handle;
    uint64_t from;
    uint32_t len;
};
struct NbdReply {
    uint32_t magic;           // 0x67446698
    uint32_t error;
    uint64_t handle;
};
#pragma pack(pop)

constexpr uint32_t kReqMagic = 0x25609513;
constexpr uint32_t kRepMagic = 0x67446698;
enum : uint16_t { CMD_READ = 0, CMD_WRITE = 1, CMD_DISC = 2, CMD_FLUSH = 3,
                  CMD_TRIM = 4, CMD_WRITE_ZEROES = 6 };

}  // namespace rmem
