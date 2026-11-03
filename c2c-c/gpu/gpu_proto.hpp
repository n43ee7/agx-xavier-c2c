// gpu_proto.hpp: wire protocol between libc2ccuda (master, lives inside the
// app) and c2c-gpud (slave, owns the actual GPU).
//
// One connection per app process (raw USB channel or TCP). Messages are
// strictly in order. "async" ops get no reply - the server remembers their
// error and hands it back on the next sync op, which is how CUDA treats async
// errors anyway. The client picks all handles (streams, events, modules,
// functions) itself so creating them never costs a round trip.
#pragma once
#include <cuda.h>

#include "../common/c2c_proto.h"  // raw channel ioctls, C2C_MAX_CHANNELS

#include <cstdint>

namespace c2cgpu {

constexpr uint16_t kDefaultPort = 10810;
constexpr char kHelloClient[8] = {'C', '2', 'C', 'G', 'P', 'U', 'C', '1'};
constexpr char kHelloServer[8] = {'C', '2', 'C', 'G', 'P', 'U', 'S', '1'};
constexpr uint32_t kProtoVersion = 2;
constexpr int kMaxAttr = 128;           // device attributes shipped in the hello
constexpr uint64_t kChunk = 8u << 20;   // staging chunk for bulk copies

// stream ids: 0 = legacy NULL stream, 2 = per-thread default, anything else
// is just the client's local handle value (unique anyway)
constexpr uint64_t kStreamLegacy = 0, kStreamPerThread = 2;

enum Op : uint32_t {
    OP_PING = 1,        // sync
    OP_MEM_ALLOC,       // sync   {va, size}: back [va, va+size) inside the window with memory
    OP_MEM_FREE,        // async  {va}
    OP_MEM_INFO,        // sync   -> {free, total}
    OP_HTOD,            // async  {dst, stream, n} + n bytes
    OP_DTOH,            // sync   {src, stream, n} -> n bytes
    OP_DTOD,            // async  {dst, src, n, stream}
    OP_MEMSET,          // async  MsgMemset
    OP_MODULE_LOAD,     // async  {mid, has_data, hash[2], n} + n bytes (if has_data)
    OP_MODULE_UNLOAD,   // async  {mid}
    OP_FUNC_GET,        // async  {fid, mid, namelen} + name
    OP_FUNC_ATTR,       // async  {fid, attr, value}
    OP_FUNC_CACHE,      // async  {fid, config}
    OP_LAUNCH,          // async  MsgLaunch + args
    OP_GLOBAL_GET,      // sync   {mid, namelen} + name -> {ptr, size}
    OP_STREAM_CREATE,   // async  {sid, flags, prio}
    OP_STREAM_DESTROY,  // async  {sid}
    OP_STREAM_SYNC,     // sync   {sid}
    OP_STREAM_QUERY,    // sync   {sid}
    OP_STREAM_WAIT,     // async  {sid, eid, flags}
    OP_EVENT_CREATE,    // async  {eid, flags}
    OP_EVENT_DESTROY,   // async  {eid}
    OP_EVENT_RECORD,    // async  {eid, sid}
    OP_EVENT_SYNC,      // sync   {eid}
    OP_EVENT_QUERY,     // sync   {eid}
    OP_EVENT_ELAPSED,   // sync   {e0, e1} -> float ms
    OP_CTX_SYNC,        // sync
    OP_CTX_LIMIT,       // async  {limit, value}
    OP_CTX_CACHE,       // async  {config}
    OP_RESET,           // sync   drop every allocation, module, stream, event
    OP_STATS,           // sync   -> MsgStats
    OP_VA_PROBE,        // sync   {size}: where would a `size` VA reservation land? -> u64
    OP_VA_WINDOW,       // sync   {base, size}: reserve the shared device-address window
    OP_ARRAY_CREATE,    // sync   {id, CUDA_ARRAY3D_DESCRIPTOR}
    OP_ARRAY_DESTROY,   // async  {id}
    OP_COPY3D,          // MsgCopy3D [+ packed host data]; sync (reply = packed data) if dst_host
    OP_TEX_CREATE,      // sync   MsgTex -> u64 texture object
    OP_TEX_DESTROY,     // async  {u64 obj}
    OP_SURF_CREATE,     // sync   CUDA_RESOURCE_DESC -> u64 surface object
    OP_SURF_DESTROY,    // async  {u64 obj}
};

// remote GPU CUarray handles, the way the app sees them
constexpr uint64_t kArrayTag = 0xC2CA000000000000ull;

#pragma pack(push, 1)
struct MsgHdr {
    uint32_t op;
    uint32_t len;  // payload bytes following this header
};
struct ReplyHdr {
    int32_t result;  // CUresult
    uint32_t len;    // payload bytes following
};
struct ServerHello {
    char magic[8];
    uint32_t version;
    int32_t driver_version;
    char name[256];
    uint8_t uuid[16];
    uint64_t total_mem;
    int32_t attr[kMaxAttr];
    uint32_t ncached;  // followed by ncached * 16-byte module hashes
};
struct MsgAlloc { uint64_t va, size; };
struct MsgCopy { uint64_t a, b, n, stream; };  // HTOD {dst,-,n,s} DTOH {src,-,n,s} DTOD {dst,src,n,s}
struct MsgMemset {
    uint64_t dst, pitch, width, height, stream;
    uint32_t value, elem;  // elem = 1, 2 or 4 bytes
};
struct MsgModule { uint32_t mid, has_data; uint64_t hash[2]; uint64_t n; };
struct MsgFunc { uint32_t fid, mid, namelen; };
struct MsgFuncAttr { uint32_t fid; int32_t attr, value; };
struct MsgLaunch {
    uint32_t fid;
    uint32_t flags;  // 1 = cooperative launch
    uint32_t grid[3], block[3];
    uint32_t shmem;
    uint64_t stream;
    uint32_t argsz;
};
struct MsgGlobal { uint32_t mid, namelen; };
struct MsgGlobalReply { uint64_t ptr, size; };
struct MsgStream { uint64_t sid; uint32_t flags; int32_t prio; };
struct MsgWait { uint64_t sid, eid; uint32_t flags; };
struct MsgEvent { uint64_t eid; uint32_t flags; };
struct MsgRecord { uint64_t eid, sid; };
struct MsgElapsed { uint64_t e0, e1; };
struct MsgLimit { int32_t which; uint64_t value; };
// CUDA_MEMCPY3D minus the host pointers. A host side goes over the wire packed
// (WidthInBytes x Height x Depth, no pitch), arrays are client ids
struct MsgCopy3D {
    CUDA_MEMCPY3D d;
    uint32_t src_host, dst_host;
    uint64_t stream;
    uint64_t packed;  // bytes of packed host data
};
struct MsgArray { uint64_t id; CUDA_ARRAY3D_DESCRIPTOR desc; };
struct MsgTex {
    CUDA_RESOURCE_DESC res;   // array handles are client ids
    CUDA_TEXTURE_DESC tex;
    uint32_t has_view;
    CUDA_RESOURCE_VIEW_DESC view;
};
struct MsgStats {
    uint64_t launches, bytes_htod, bytes_dtoh, allocs, alloc_bytes, modules_loaded, modules_cached;
};
#pragma pack(pop)

}  // namespace c2cgpu
