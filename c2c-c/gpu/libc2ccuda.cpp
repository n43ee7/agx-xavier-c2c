// libc2ccuda: makes the slave's GPU show up as a normal CUDA device on the master.
//
// It gets installed as libcuda.so.1 in its own dir (c2c-gpu run puts that on
// LD_LIBRARY_PATH), so unmodified CUDA apps (runtime API, cuBLAS, static
// cudart, whatever) load us instead of NVIDIA's libcuda. We load the real
// driver ourselves and pass local GPU calls straight through. The remote GPU
// is added after the local ones (dev 0 = this Xavier, dev 1 = slave), and we
// handle CUDA_VISIBLE_DEVICES over the whole list.
//
// How the remote GPU works, roughly:
//  - "shadow context": each remote ctx has a real ctx on the local GPU behind
//    it. Both boards are GV11B, so queries, module bookkeeping, the occupancy
//    calculator and NVIDIA's undocumented tables all get the right answer
//    locally.
//  - the data plane (allocs, copies, memsets, launches, streams, events,
//    syncs) goes to c2c-gpud on the slave.
//  - device pointers are unique across both GPUs. Every remote alloc is
//    reserved in our local VA space and mapped at the same address on the
//    slave, so pointers inside structs / kernel args just work.
//  - kernel args get serialised with the layout we pull out of the
//    cubin/PTX (fatbin.cpp).
//  - creating streams/events/modules/functions and launching never wait on
//    the link. Only calls that return data or have to wait do a round trip.
//  - anything we don't handle on a remote ctx returns
//    CUDA_ERROR_NOT_SUPPORTED rather than silently running on the wrong GPU.
#include <cuda.h>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <strings.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fatbin.hpp"
#include "gpu_proto.hpp"
#include "tramp_names.inc"

#define C2CAPI extern "C" __attribute__((visibility("default")))

using namespace c2cgpu;

// ---------------------------------------------------------------- real driver

static void* g_lib = nullptr;
static void* g_real[kNumSyms];
static std::unordered_map<void*, unsigned> g_real_index;  // real fn ptr -> index into kSymNames
extern "C" void* c2c_tramp_table[];

static int g_debug = 0;
static void dbg(const char* fmt, ...) {
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[c2c-gpu] ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}
static void warnf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[c2c-gpu] ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

static unsigned sym_index(const char* name) {
    for (unsigned i = 0; i < kNumSyms; i++)
        if (!std::strcmp(kSymNames[i], name)) return i;
    return ~0u;
}

static void load_real() {
    if (g_lib) return;
    const char* env = getenv("C2C_REAL_LIBCUDA");
    const char* cands[] = {env, "/usr/lib/aarch64-linux-gnu/tegra/libcuda.so.1.1",
                           "/usr/lib/aarch64-linux-gnu/tegra/libcuda.so.1",
                           "/usr/lib/aarch64-linux-gnu/libcuda.so.1",
                           "/usr/lib/x86_64-linux-gnu/libcuda.so.1"};
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&load_real), &self);
    for (const char* c : cands) {
        if (!c || access(c, R_OK) != 0) continue;
        char a[4096], b[4096];
        if (self.dli_fname && realpath(c, a) && realpath(self.dli_fname, b) && !std::strcmp(a, b))
            continue;  // that's us, skip
        g_lib = dlopen(c, RTLD_NOW | RTLD_LOCAL);
        if (g_lib) break;
    }
    if (!g_lib) {
        warnf("cannot load NVIDIA's libcuda (set C2C_REAL_LIBCUDA)");
        return;
    }
    for (unsigned i = 0; i < kNumSyms; i++) {
        g_real[i] = dlsym(g_lib, kSymNames[i]);
        if (g_real[i]) g_real_index.emplace(g_real[i], i);
    }
}

template <class F>
static F real_fn(const char* name) {
    load_real();
    unsigned i = sym_index(name);
    return reinterpret_cast<F>(i == ~0u ? dlsym(g_lib, name) : g_real[i]);
}
// REAL(cuMemAlloc_v2)(...) calls NVIDIA's version.
// Careful: #fn is stringified BEFORE cuda.h's macros expand, so REAL(cuMemcpyDtoH)
// looks up the old v1 symbol (32-bit pointers!). Always spell out the _v2 name.
#define REAL(fn) ([]() { static auto p = real_fn<decltype(&fn)>(#fn); return p; }())

// ------------------------------------------------------------- remote state

namespace {

enum class Kind { Alloc, Small, Alias };
struct Range {
    uint64_t size;      // size the caller asked for (Alias: symbol size)
    uint64_t reserved;  // what we actually reserved locally (Alloc)
    Kind kind;
    uint64_t target;    // Alias: address on the slave, Small: slab base
};
struct Slab {
    uint64_t base, used = 0;
    unsigned live = 0;
};

struct Mod {
    uint32_t mid;
    std::vector<uint8_t> owned;   // our copy of an image the app passed in
    const void* image = nullptr;  // registered fatbin, stays valid
    bool parsed = false, sent = false, bad = false;
    ParsedImage P;
    uint64_t hash[2] = {0, 0};
};
struct Func {
    Mod* mod;
    std::string name;
    uint32_t fid;
    bool sent = false;
    const ParamLayout* L = nullptr;
    std::vector<std::pair<int, int>> attrs;  // applied before the first launch
    int cache = -1;
};

// one outstanding reply. Server answers strictly in request order
struct Pending {
    void* out = nullptr;           // payload lands here (max outn bytes)
    size_t outn = 0;
    bool detached = false;         // nobody waits on it, reader deletes it
    std::atomic<bool> done{false};
    int32_t result = 0;
};

struct State {
    std::recursive_mutex mu;
    bool inited = false;
    int fd = -1;
    std::atomic<bool> dead{false};
    // raw = USB channel /dev/c2cchN, otherwise TCP over c2c0
    bool raw = false;
    std::string via;
    int spin_us = 1000;            // spin this long before going to sleep
    // queue of Pendings, filled in order as replies come in
    std::mutex pmu;
    std::condition_variable pcv;
    std::deque<Pending*> pend;
    std::mutex rxmu;                       // whoever holds this reads the fd
    int detached_out = 0;                  // # detached downloads in flight (under pmu)
    std::atomic<int32_t> deferred_err{0};  // error from a detached download, reported later
    // sync skipping. post_seq counts every request we send. If a sync call
    // just drained stream clean_sid (or everything) and nothing went out since,
    // the next cuStreamSynchronize is a no-op. cudart does a stream sync after
    // every pageable D2H cudaMemcpy, so this turns 2 round trips into 1.
    uint64_t post_seq = 0, clean_seq = ~0ull, clean_sid = ~0ull;
    bool clean_all = false;
    // pinned bounce buffers for remote <-> local GPU copies
    void* stage[4] = {nullptr, nullptr, nullptr, nullptr};
    ServerHello hello{};
    std::set<std::pair<uint64_t, uint64_t>> server_has;  // module hashes the slave already has cached
    int nlocal = 0;
    // visible ordinal -> real local ordinal, or -1 for the remote one
    std::vector<int> vis;
    int remote_ord = -1;  // which visible ordinal is the remote GPU
    // shadow ctxs (local ctxs standing in for remote ones)
    std::unordered_set<CUcontext> shadow;
    CUcontext prim = nullptr;
    int prim_ref = 0;
    unsigned prim_flags = 0;
    std::map<uint64_t, Range> ranges;
    std::map<uint64_t, Slab> slabs;
    uint64_t cur_slab = 0;
    uint64_t alias_base = 0, alias_next = 0, alias_end = 0;
    // VA window shared w/ the slave: [win_base, win_base + win_size)
    uint64_t win_base = 0, win_size = 0;
    bool win_failed = false;
    std::map<uint64_t, uint64_t> win_free;  // free ranges, start -> size
    std::map<std::pair<Mod*, std::string>, uint64_t> alias_of;
    std::unordered_map<CUmodule, Mod*> mods;
    std::unordered_map<CUfunction, Func*> funcs;
    std::unordered_set<CUstream> streams;
    std::unordered_set<CUevent> events;
    uint32_t next_id = 1;
    std::vector<uint8_t> argbuf;
    // async msgs not sent yet. They go out with the next sync call, once 256K
    // piles up, or after ~100us via the flusher thread. Way fewer writes.
    std::vector<uint8_t> out;
    std::condition_variable_any cv;
    // stats
    uint64_t n_launch = 0, n_rpc = 0, b_up = 0, b_down = 0, n_async_dtoh = 0;
};
State& S() {
    static State* s = new State;  // leaked on purpose, atexit handlers still use it
    return *s;
}

constexpr uint64_t kGran = 2u << 20;
constexpr uint64_t kSmallMax = 1u << 20;
constexpr uint64_t kSmallAlign = 512;

uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

void hash128(const uint8_t* p, size_t n, uint64_t out[2]) {
    uint64_t a = 1469598103934665603ull, b = 0x9e3779b97f4a7c15ull ^ n;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ p[i]) * 1099511628211ull;
        b = (b ^ p[i]) * 0xff51afd7ed558ccdull;
        b ^= b >> 29;
    }
    out[0] = a;
    out[1] = b;
}

// ------------------------------------------------------------------ network
//
// Same byte stream to c2c-gpud, two ways to carry it:
//   raw  USB channel /dev/c2cchN. Our process -> USB driver -> c2c-gpud,
//        nothing else in between (no TCP/IP, TUN or c2cd). ~half the RTT.
//   tcp  TCP over c2c0. Fallback when all channels are taken or
//        C2C_GPU_TRANSPORT=tcp. Survives short link cuts, raw doesn't.
// Replies come back in request order. *Async downloads into pinned memory
// don't block, the data streams back while we keep sending (full duplex).

double now_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

bool wr_all(int fd, const void* p, size_t n) {
    State& s = S();
    auto* c = static_cast<const uint8_t*>(p);
    while (n) {
        ssize_t r = s.raw ? write(fd, c, n) : send(fd, c, n, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && errno == EAGAIN) {  // raw: every USB request is in flight
                pollfd pf{fd, POLLOUT, 0};
                poll(&pf, 1, 100);
                continue;
            }
            return false;
        }
        c += r;
        n -= size_t(r);
    }
    return true;
}
bool rd_all(int fd, void* p, size_t n) {
    State& s = S();
    auto* c = static_cast<uint8_t*>(p);
    double spin_from = 0;
    while (n) {
        ssize_t r = s.raw ? read(fd, c, n) : recv(fd, c, n, MSG_WAITALL);
        if (r > 0) {
            c += r;
            n -= size_t(r);
            spin_from = 0;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) {  // raw fd is nonblocking: spin a bit, then poll
            if (s.spin_us > 0) {
                double t = now_us();
                if (spin_from == 0) spin_from = t;
                if (t - spin_from < s.spin_us) continue;
            }
            pollfd pf{fd, POLLIN, 0};
            poll(&pf, 1, 100);
            continue;
        }
        return false;
    }
    return true;
}
constexpr CUresult kLinkErr = CUDA_ERROR_LAUNCH_FAILED;

void link_lost() {
    State& s = S();
    if (!s.dead.exchange(true)) warnf("connection to the remote GPU lost");
}

bool flush() {
    State& s = S();
    if (s.out.empty() || s.dead) {
        s.out.clear();
        return !s.dead;
    }
    bool ok = wr_all(s.fd, s.out.data(), s.out.size());
    s.out.clear();
    if (!ok) link_lost();
    return ok;
}

constexpr size_t kBatchMax = 256u << 10, kInlineMax = 64u << 10;

// fire-and-forget msg: header + struct + optional payload. caller holds s.mu
bool post(uint32_t op, const void* m, size_t mn, const void* extra = nullptr, size_t en = 0) {
    State& s = S();
    if (s.dead) return false;
    s.post_seq++;
    MsgHdr h{op, uint32_t(mn + (en > 0xffffffffu ? 0 : en))};
    if (en > kInlineMax) {  // big payloads are written straight from the caller's buffer
        if (!flush()) return false;
        uint8_t hm[sizeof h + 256];
        std::memcpy(hm, &h, sizeof h);
        if (mn) std::memcpy(hm + sizeof h, m, mn);
        if (!wr_all(s.fd, hm, sizeof h + mn) || !wr_all(s.fd, extra, en)) {
            link_lost();
            return false;
        }
        return true;
    }
    bool was_empty = s.out.empty();
    auto put = [&](const void* p, size_t n) {
        auto* c = static_cast<const uint8_t*>(p);
        s.out.insert(s.out.end(), c, c + n);
    };
    put(&h, sizeof h);
    if (mn) put(m, mn);
    if (en) put(extra, en);
    if (s.out.size() >= kBatchMax) return flush();
    if (was_empty) s.cv.notify_one();
    return true;
}

void flusher() {
    State& s = S();
    for (;;) {
        {
            std::unique_lock<std::recursive_mutex> lk(s.mu);
            s.cv.wait(lk, [&] { return !s.out.empty() || s.dead; });
            if (s.dead) return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        std::lock_guard<std::recursive_mutex> g(s.mu);
        flush();
    }
}

// Leader/follower: whoever is waiting for a reply grabs the reader lock and
// reads replies itself, in order, passing other threads' replies along. So
// the normal path has no thread handoff (that handoff cost ~90us when we had
// a dedicated rx thread). The background reader only runs while detached
// downloads are pending, so their data keeps flowing while the app sends.
void fail_all_pending() {
    State& s = S();
    link_lost();
    std::lock_guard<std::mutex> g(s.pmu);
    for (Pending* p : s.pend) {
        if (p->detached) {
            s.detached_out--;
            delete p;
        } else {
            p->result = kLinkErr;
            p->done.store(true, std::memory_order_release);
        }
    }
    s.pend.clear();
    s.pcv.notify_all();
}

// read + deliver one reply, caller holds rxmu. false means the link died
bool read_one_reply() {
    State& s = S();
    thread_local std::vector<uint8_t> junk(64u << 10);
    ReplyHdr r;
    if (s.dead || !rd_all(s.fd, &r, sizeof r)) {
        fail_all_pending();
        return false;
    }
    Pending* p = nullptr;
    {
        std::lock_guard<std::mutex> g(s.pmu);
        if (!s.pend.empty()) p = s.pend.front();
    }
    if (!p) {
        warnf("unexpected reply from the remote GPU (protocol error)");
        fail_all_pending();
        return false;
    }
    size_t take = std::min<size_t>(r.len, p->outn);
    bool ok = !take || rd_all(s.fd, p->out, take);
    for (size_t left = r.len - take; ok && left;) {  // throw away anything extra
        size_t c = std::min(left, junk.size());
        ok = rd_all(s.fd, junk.data(), c);
        left -= c;
    }
    if (!ok) {
        fail_all_pending();
        return false;
    }
    {
        std::lock_guard<std::mutex> g(s.pmu);
        s.pend.pop_front();
        if (p->detached) {
            if (r.result) {
                int32_t zero = 0;
                s.deferred_err.compare_exchange_strong(zero, r.result);
            }
            s.detached_out--;
            delete p;
        } else {
            p->result = r.result;
            p->done.store(true, std::memory_order_release);
        }
    }
    s.pcv.notify_all();
    return true;
}

void rx_loop() {
    State& s = S();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.pmu);
            s.pcv.wait(lk, [&] { return s.detached_out > 0 || s.dead; });
            if (s.dead) return;
        }
        std::lock_guard<std::mutex> g(s.rxmu);
        while (s.detached_out > 0 && !s.dead)
            if (!read_one_reply()) return;
    }
}

// queue the reply slot BEFORE sending, the reply can beat us otherwise
void expect(Pending* p) {
    State& s = S();
    std::lock_guard<std::mutex> g(s.pmu);
    if (s.dead) {
        p->result = kLinkErr;
        p->done.store(true);
        if (p->detached) delete p;
        return;
    }
    if (p->detached) s.detached_out++;
    s.pend.push_back(p);
    if (p->detached) s.pcv.notify_all();  // kick the background reader
}
CUresult wait_for(Pending& p) {
    State& s = S();
    double t0 = now_us();
    while (!p.done.load(std::memory_order_acquire)) {
        // nobody's reading, so we do it (ours or older ones first)
        if (s.rxmu.try_lock()) {
            if (!p.done.load(std::memory_order_acquire)) read_one_reply();
            s.rxmu.unlock();
            continue;
        }
        if (s.spin_us > 0 && now_us() - t0 < s.spin_us) continue;
        std::unique_lock<std::mutex> lk(s.pmu);
        s.pcv.wait_for(lk, std::chrono::milliseconds(20),
                       [&] { return p.done.load(std::memory_order_acquire); });
    }
    CUresult r = CUresult(p.result);
    if (r == CUDA_SUCCESS) r = CUresult(s.deferred_err.exchange(0));
    return r;
}

// send a request and wait for the reply; payload (max outn bytes) goes to out
CUresult call(uint32_t op, const void* m, size_t mn, void* out = nullptr, size_t outn = 0,
              const void* extra = nullptr, size_t en = 0) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (s.dead) return kLinkErr;
    Pending p;
    p.out = out;
    p.outn = outn;
    expect(&p);
    if (!post(op, m, mn, extra, en) || !flush()) {
        link_lost();
        // the reader may be stuck in read(), so fail this one ourselves
        std::lock_guard<std::mutex> gp(s.pmu);
        for (auto it = s.pend.begin(); it != s.pend.end(); ++it)
            if (*it == &p) {
                s.pend.erase(it);
                break;
            }
        return kLinkErr;
    }
    s.n_rpc++;
    return wait_for(p);
}

// read the server hello. needs a timeout, otherwise a raw channel with no
// server behind it hangs us forever
bool handshake(int fd) {
    State& s = S();
    uint32_t ver = kProtoVersion;
    uint8_t hello[12];
    std::memcpy(hello, kHelloClient, 8);
    std::memcpy(hello + 8, &ver, 4);
    if (!wr_all(fd, hello, sizeof hello)) return false;
    int ms = getenv("C2C_GPU_CONNECT_MS") ? std::atoi(getenv("C2C_GPU_CONNECT_MS")) : 1500;
    pollfd pf{fd, POLLIN, 0};
    if (poll(&pf, 1, ms) != 1 || !(pf.revents & POLLIN)) return false;
    if (!rd_all(fd, &s.hello, sizeof s.hello) ||
        std::memcmp(s.hello.magic, kHelloServer, 8) != 0)
        return false;
    for (uint32_t i = 0; i < s.hello.ncached; i++) {
        uint64_t h[2];
        if (!rd_all(fd, h, 16)) return false;
        s.server_has.insert({h[0], h[1]});
    }
    return true;
}

// grab the first free raw channel that has a server answering
bool connect_raw() {
    State& s = S();
    for (int i = 0; i < C2C_MAX_CHANNELS; i++) {
        char path[32];
        std::snprintf(path, sizeof path, "/dev/c2cch%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) break;       // ran out of channels
            continue;                          // EBUSY: someone else has it
        }
        s.raw = true;
        s.server_has.clear();
        if (handshake(fd)) {
            s.fd = fd;
            s.via = path;
            return true;
        }
        dbg("no remote GPU server on %s", path);
        s.raw = false;
        close(fd);
    }
    return false;
}

bool connect_tcp() {
    State& s = S();
    std::string target = getenv("C2C_GPU_SERVER") ? getenv("C2C_GPU_SERVER") : "10.77.0.2";
    int port = kDefaultPort;
    size_t colon = target.rfind(':');
    if (colon != std::string::npos) {
        port = std::atoi(target.c_str() + colon + 1);
        target.resize(colon);
    }
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(target.c_str(), std::to_string(port).c_str(), &hints, &ai) != 0) return false;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // nonblocking connect w/ short timeout. no link -> just the local GPUs
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        int ms = getenv("C2C_GPU_CONNECT_MS") ? std::atoi(getenv("C2C_GPU_CONNECT_MS")) : 1500;
        if (poll(&p, 1, ms) == 1) {
            int err = 0;
            socklen_t l = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
            rc = err ? -1 : 0;
        }
    }
    fcntl(fd, F_SETFL, fl);
    if (rc != 0) {
        dbg("no remote GPU at %s:%d", target.c_str(), port);
        close(fd);
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    s.raw = false;
    s.server_has.clear();
    if (!handshake(fd)) {
        warnf("remote GPU server at %s:%d did not answer the handshake", target.c_str(), port);
        close(fd);
        return false;
    }
    s.fd = fd;
    s.via = "tcp " + target + ":" + std::to_string(port);
    return true;
}

// settings: env var wins, then /etc/c2c/c2c.conf
std::string conf_get(const char* env, const char* key, const char* def) {
    if (const char* e = getenv(env)) return e;
    std::string v = def;
    if (FILE* f = std::fopen("/etc/c2c/c2c.conf", "r")) {
        char line[512];
        size_t kl = std::strlen(key);
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (std::strncmp(p, key, kl) != 0) continue;
            p += kl;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            std::string val(p);
            while (!val.empty() && std::strchr(" \t\r\n#", val.back())) val.pop_back();
            size_t hash = val.find('#');
            if (hash != std::string::npos) val.resize(hash);
            while (!val.empty() && std::strchr(" \t", val.back())) val.pop_back();
            v = val;
        }
        std::fclose(f);
    }
    return v;
}

// c2cd writes the link state here. If it says DOWN don't bother waiting on a
// connect timeout, just start with the local GPUs
bool link_known_down() {
    FILE* f = std::fopen("/run/c2c/status.json", "r");
    if (!f) return false;  // no idea, so try anyway
    char b[256] = {0};
    size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    b[n] = 0;
    return std::strstr(b, "\"state\":\"UP\"") == nullptr;
}

bool connect_remote() {
    State& s = S();
    if (getenv("C2C_GPU_DISABLE")) return false;
    std::string tr = conf_get("C2C_GPU_TRANSPORT", "gpu_transport", "auto");
    s.spin_us = std::atoi(conf_get("C2C_GPU_SPIN_US", "gpu_spin_us", "1000").c_str());
    bool ok = false;
    if (getenv("C2C_GPU_SERVER")) tr = "tcp";  // explicit address means TCP
    if (tr == "raw" || tr == "auto") ok = connect_raw();  // fails instantly if there's no USB
    // raw channels don't need c2cd but TCP does, so skip the TCP timeout when
    // c2cd says the link is down
    if (!ok && (tr == "tcp" || tr == "auto")) {
        if (link_known_down()) dbg("C2C link is down: local GPUs only");
        else ok = connect_tcp();
    }
    if (!ok) return false;
    std::thread(rx_loop).detach();
    std::thread(flusher).detach();
    dbg("remote GPU '%s' (%.1f GiB) via %s, %u cached modules, spin %d us", s.hello.name,
        s.hello.total_mem / 1073741824.0, s.via.c_str(), s.hello.ncached, s.spin_us);
    return true;
}

// ------------------------------------------------------------ context logic

CUcontext current_ctx() {
    CUcontext c = nullptr;
    REAL(cuCtxGetCurrent)(&c);
    return c;
}
bool is_shadow(CUcontext c) {
    State& s = S();
    return c && s.fd >= 0 && s.shadow.count(c);
}
bool on_remote() {
    State& s = S();
    if (s.fd < 0) return false;
    std::lock_guard<std::recursive_mutex> g(s.mu);
    return is_shadow(current_ctx());
}

const Range* find_range(uint64_t p, uint64_t* base = nullptr) {
    State& s = S();
    if (s.ranges.empty()) return nullptr;
    auto it = s.ranges.upper_bound(p);
    if (it == s.ranges.begin()) return nullptr;
    --it;
    uint64_t span = it->second.kind == Kind::Alloc ? it->second.reserved : it->second.size;
    if (p >= it->first + std::max<uint64_t>(span, 1)) return nullptr;
    if (base) *base = it->first;
    return &it->second;
}
bool is_remote_ptr(uint64_t p) {
    State& s = S();
    if (s.fd < 0) return false;
    std::lock_guard<std::recursive_mutex> g(s.mu);
    return find_range(p) != nullptr;
}
// app-visible address -> slave address (only differs for aliases)
uint64_t xlate(uint64_t p) {
    uint64_t base;
    const Range* r = find_range(p, &base);
    if (r && r->kind == Kind::Alias) return r->target + (p - base);
    return p;
}

bool is_remote_stream(CUstream st) {
    State& s = S();
    if (s.fd < 0) return false;
    if (st == nullptr || st == CU_STREAM_LEGACY || st == CU_STREAM_PER_THREAD)
        return is_shadow(current_ctx());
    return s.streams.count(st) != 0;
}
uint64_t sid_of(CUstream st) {
    if (st == nullptr || st == CU_STREAM_LEGACY) return kStreamLegacy;
    if (st == CU_STREAM_PER_THREAD) return kStreamPerThread;
    return reinterpret_cast<uint64_t>(st);
}

enum class Where { Remote, LocalDev, Host };
Where where(uint64_t p) {
    if (is_remote_ptr(p)) return Where::Remote;
    unsigned type = 0;
    int managed = 0;
    if (REAL(cuPointerGetAttribute)(&type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, CUdeviceptr(p)) ==
            CUDA_SUCCESS &&
        type == CU_MEMORYTYPE_DEVICE) {
        REAL(cuPointerGetAttribute)(&managed, CU_POINTER_ATTRIBUTE_IS_MANAGED, CUdeviceptr(p));
        if (!managed) return Where::LocalDev;
    }
    return Where::Host;
}

// ------------------------------------------------------------- data plane

CUresult r_htod(uint64_t dst, const void* src, size_t n, uint64_t sid) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (!n) return CUDA_SUCCESS;
    MsgCopy m{xlate(dst), 0, n, sid};
    s.b_up += n;
    return post(OP_HTOD, &m, sizeof m, src, n) ? CUDA_SUCCESS : kLinkErr;
}
CUresult r_dtoh(void* dst, uint64_t src, size_t n, uint64_t sid) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    MsgCopy m{xlate(src), 0, n, sid};
    s.b_down += n;
    CUresult r = call(OP_DTOH, &m, sizeof m, dst, n);
    if (r == CUDA_SUCCESS) {  // server waited for everything queued before it on sid
        s.clean_seq = s.post_seq;
        s.clean_sid = sid;
        s.clean_all = false;
    }
    return r;
}
// kick off a download. data lands in dst when the reply shows up, which is in
// stream order, i.e. before the reply to any later sync call
Pending* r_dtoh_begin(void* dst, uint64_t src, size_t n, uint64_t sid, bool detached) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    MsgCopy m{xlate(src), 0, n, sid};
    s.b_down += n;
    auto* p = new Pending;
    p->out = dst;
    p->outn = n;
    p->detached = detached;
    expect(p);
    if (!post(OP_DTOH, &m, sizeof m) || !flush()) link_lost();
    return p;
}
CUresult r_dtoh_end(Pending* p) {
    CUresult r = wait_for(*p);
    delete p;
    return r;
}
bool is_pinned(const void* p) {
    unsigned int type = 0;
    return REAL(cuPointerGetAttribute)(&type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, CUdeviceptr(p)) ==
               CUDA_SUCCESS &&
           type == CU_MEMORYTYPE_HOST;
}
// same rules as real cuMemcpyDtoHAsync: pinned dst = actually async (overlaps
// whatever we send next), pageable dst = synchronous
CUresult r_dtoh_async(void* dst, uint64_t src, size_t n, uint64_t sid) {
    if (!n) return CUDA_SUCCESS;
    if (!is_pinned(dst)) return r_dtoh(dst, src, n, sid);
    State& s = S();
    if (s.dead) return kLinkErr;
    s.n_async_dtoh++;
    r_dtoh_begin(dst, src, n, sid, true);
    return s.dead ? kLinkErr : CUDA_SUCCESS;
}
CUresult r_dtod(uint64_t dst, uint64_t src, size_t n, uint64_t sid) {
    std::lock_guard<std::recursive_mutex> g(S().mu);
    if (!n) return CUDA_SUCCESS;
    MsgCopy m{xlate(dst), xlate(src), n, sid};
    return post(OP_DTOD, &m, sizeof m) ? CUDA_SUCCESS : kLinkErr;
}
CUresult r_memset(uint64_t dst, uint64_t pitch, uint32_t v, uint32_t elem, size_t w, size_t h,
                  uint64_t sid) {
    std::lock_guard<std::recursive_mutex> g(S().mu);
    MsgMemset m{xlate(dst), pitch, w, h, sid, v, elem};
    return post(OP_MEMSET, &m, sizeof m) ? CUDA_SUCCESS : kLinkErr;
}

// copy between any two kinds of memory. remote <-> local GPU has to bounce
// through host memory
CUresult copy_any(uint64_t dst, uint64_t src, size_t n, CUstream st, bool async) {
    Where wd = where(dst), ws = where(src);
    if (wd != Where::Remote && ws != Where::Remote)
        return async ? REAL(cuMemcpyAsync)(dst, src, n, st) : REAL(cuMemcpy)(dst, src, n);
    uint64_t sid = is_remote_stream(st) ? sid_of(st) : kStreamLegacy;
    if (wd == Where::Remote && ws == Where::Remote) return r_dtod(dst, src, n, sid);
    if (wd == Where::Remote && ws == Where::Host)
        return r_htod(dst, reinterpret_cast<const void*>(src), n, sid);
    if (wd == Where::Host && ws == Where::Remote)
        return async ? r_dtoh_async(reinterpret_cast<void*>(dst), src, n, sid)
                     : r_dtoh(reinterpret_cast<void*>(dst), src, n, sid);
    // remote <-> local GPU: pinned bounce buffers, pipelined. a few chunks are
    // on the wire while finished ones get copied to the local GPU (fast)
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    constexpr size_t kStage = 4u << 20;
    constexpr int kN = 4;
    if (!s.stage[0])
        for (int i = 0; i < kN; i++)
            if (REAL(cuMemHostAlloc)(&s.stage[i], kStage, 0) != CUDA_SUCCESS) {
                s.stage[i] = nullptr;
                return CUDA_ERROR_OUT_OF_MEMORY;
            }
    REAL(cuCtxSynchronize)();
    CUresult r = CUDA_SUCCESS;
    if (ws == Where::Remote) {
        Pending* inflight[kN] = {};
        size_t nchunks = (n + kStage - 1) / kStage, issued = 0;
        auto issue = [&](size_t k) {
            size_t off = k * kStage, c = std::min(kStage, n - off);
            inflight[k % kN] = r_dtoh_begin(s.stage[k % kN], src + off, c, sid, false);
        };
        while (issued < nchunks && issued < size_t(kN)) issue(issued++);
        for (size_t k = 0; k < nchunks; k++) {
            CUresult e = r_dtoh_end(inflight[k % kN]);
            inflight[k % kN] = nullptr;
            size_t off = k * kStage, c = std::min(kStage, n - off);
            if (!e && !r) e = REAL(cuMemcpyHtoD_v2)(dst + off, s.stage[k % kN], c);
            if (e && !r) r = e;
            if (issued < nchunks && !r) issue(issued++);
        }
        for (auto* p : inflight)
            if (p) r_dtoh_end(p);
        return r;
    }
    // local -> remote: once post() returns the kernel has the chunk, so the
    // buffer can be reused immediately
    for (size_t off = 0, k = 0; off < n && !r; off += kStage, k++) {
        size_t c = std::min(kStage, n - off);
        r = REAL(cuMemcpyDtoH_v2)(s.stage[k % kN], src + off, c);
        if (!r) r = r_htod(dst + off, s.stage[k % kN], c, sid);
    }
    if (!r && !async) r = call(OP_STREAM_SYNC, &sid, 8);
    return r;
}

// ---------------------------------------------------------------- memory

// Pick one GPU VA window that's free on both sides. CUDA puts big
// reservations right after its heap, and that spot differs between the 16GB
// and 32GB Xavier. Whoever's free area starts lower pads up to the other's
// start with a filler reservation, then both reserve the same range. Local
// allocs can never land in it, so remote pointers are unique here.
bool setup_window() {
    State& s = S();
    if (s.win_size) return true;
    if (s.win_failed) return false;
    s.win_failed = true;
    uint64_t total = s.hello.total_mem;
    uint64_t size = round_up(total, 1ull << 30) + (4ull << 30);
    CUdeviceptr mine = 0;
    CUresult rr = REAL(cuMemAddressReserve)(&mine, size, 0, 0, 0);
    if (rr) {
        warnf("cannot reserve %llu GiB of GPU address space for the remote GPU (error %d)",
              (unsigned long long)(size >> 30), int(rr));
        return false;
    }
    REAL(cuMemAddressFree)(mine, size);
    uint64_t theirs = 0;
    if (call(OP_VA_PROBE, &size, 8, &theirs, 8)) return false;
    uint64_t base = std::max<uint64_t>(mine, theirs);
    CUdeviceptr filler = 0, got = 0;
    if (mine < base) {
        if (REAL(cuMemAddressReserve)(&filler, base - mine, 0, 0, 0) || filler != mine) {
            warnf("cannot align the device-address window (local %#llx, remote %#llx)",
                  (unsigned long long)mine, (unsigned long long)theirs);
            if (filler) REAL(cuMemAddressFree)(filler, base - mine);
            return false;
        }
    }
    if (REAL(cuMemAddressReserve)(&got, size, 0, base, 0) || got != base) {
        if (got) REAL(cuMemAddressFree)(got, size);
        warnf("cannot reserve the device-address window at %#llx", (unsigned long long)base);
        return false;
    }
    MsgAlloc m{base, size};
    CUresult r = call(OP_VA_WINDOW, &m, sizeof m);
    if (r) {
        warnf("remote GPU could not reserve the device-address window (error %d)", int(r));
        return false;
    }
    // filler stays reserved until exit (just VA, no memory)
    s.win_base = base;
    s.win_size = size;
    s.win_free[base] = size;
    s.win_failed = false;
    dbg("device-address window %#llx + %llu GiB", (unsigned long long)base,
        (unsigned long long)(size >> 30));
    return true;
}

uint64_t win_take(uint64_t size) {
    State& s = S();
    for (auto it = s.win_free.begin(); it != s.win_free.end(); ++it) {
        if (it->second < size) continue;
        uint64_t p = it->first, left = it->second - size;
        s.win_free.erase(it);
        if (left) s.win_free[p + size] = left;
        return p;
    }
    return 0;
}

void win_give(uint64_t p, uint64_t size) {
    State& s = S();
    auto it = s.win_free.emplace(p, size).first;
    auto nx = std::next(it);
    if (nx != s.win_free.end() && it->first + it->second == nx->first) {
        it->second += nx->second;
        s.win_free.erase(nx);
    }
    if (it != s.win_free.begin()) {
        auto pv = std::prev(it);
        if (pv->first + pv->second == it->first) {
            pv->second += it->second;
            s.win_free.erase(it);
        }
    }
}

CUresult r_alloc_big(CUdeviceptr* out, uint64_t n) {
    State& s = S();
    if (!setup_window()) return CUDA_ERROR_OUT_OF_MEMORY;
    uint64_t size = round_up(std::max<uint64_t>(n, 1), kGran);
    uint64_t va = win_take(size);
    if (!va) return CUDA_ERROR_OUT_OF_MEMORY;
    MsgAlloc m{va, size};
    CUresult r = call(OP_MEM_ALLOC, &m, sizeof m);
    if (r) {
        win_give(va, size);
        return r;
    }
    s.ranges[va] = {n, size, Kind::Alloc, 0};
    *out = va;
    dbg("alloc %llu bytes -> %#llx", (unsigned long long)n, (unsigned long long)va);
    return CUDA_SUCCESS;
}

CUresult r_free_big(uint64_t va) {
    State& s = S();
    auto it = s.ranges.find(va);
    if (it == s.ranges.end()) return CUDA_ERROR_INVALID_VALUE;
    uint64_t reserved = it->second.reserved;
    s.ranges.erase(it);
    post(OP_MEM_FREE, &va, 8);
    // slave unmaps before it processes anything we send after this, so the
    // range can be reused right away
    win_give(va, reserved);
    return CUDA_SUCCESS;
}

CUresult r_alloc(CUdeviceptr* out, uint64_t n) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (!out) return CUDA_ERROR_INVALID_VALUE;
    if (n == 0) return CUDA_ERROR_INVALID_VALUE;
    if (n > kSmallMax) return r_alloc_big(out, n);
    uint64_t need = round_up(n, kSmallAlign);
    auto cs = s.slabs.find(s.cur_slab);
    if (cs == s.slabs.end() || cs->second.used + need > kGran) {
        CUdeviceptr base;
        CUresult r = r_alloc_big(&base, kGran);
        if (r) return r;
        s.slabs[base] = Slab{base};
        // the slab isn't an app allocation, keep it out of range lookups
        s.ranges.erase(base);
        // drop the old slab if nothing in it is still alive
        auto old = s.slabs.find(s.cur_slab);
        if (old != s.slabs.end() && old->second.live == 0) {
            uint64_t ob = old->first;
            s.slabs.erase(old);
            s.ranges[ob] = {kGran, kGran, Kind::Alloc, 0};
            r_free_big(ob);
        }
        s.cur_slab = base;
        cs = s.slabs.find(base);
    }
    uint64_t p = cs->first + cs->second.used;
    cs->second.used += need;
    cs->second.live++;
    s.ranges[p] = {n, need, Kind::Small, cs->first};
    *out = p;
    return CUDA_SUCCESS;
}

CUresult r_free(uint64_t p) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    auto it = s.ranges.find(p);
    if (it == s.ranges.end()) return CUDA_ERROR_INVALID_VALUE;
    if (it->second.kind == Kind::Alloc) return r_free_big(p);
    if (it->second.kind != Kind::Small) return CUDA_ERROR_INVALID_VALUE;
    uint64_t slab = it->second.target;
    s.ranges.erase(it);
    auto sl = s.slabs.find(slab);
    if (sl != s.slabs.end() && --sl->second.live == 0 && slab != s.cur_slab) {
        s.slabs.erase(sl);
        s.ranges[slab] = {kGran, kGran, Kind::Alloc, 0};
        r_free_big(slab);
    }
    return CUDA_SUCCESS;
}

// ---------------------------------------------------------------- modules

Mod* register_module(CUmodule m, const void* image, bool persistent) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    Mod* md = new Mod;
    md->mid = s.next_id++;
    if (persistent) {
        md->image = image;
    } else {
        size_t n = image_size(image);
        auto* p = static_cast<const uint8_t*>(image);
        md->owned.assign(p, p + n);
        md->image = md->owned.data();
    }
    auto old = s.mods.find(m);
    if (old != s.mods.end()) delete old->second;  // driver recycled the handle
    s.mods[m] = md;
    dbg("module %u registered", md->mid);
    return md;
}

int remote_sm() {
    State& s = S();
    return s.hello.attr[CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR] * 10 +
           s.hello.attr[CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR];
}

bool ensure_parsed(Mod* md) {
    if (md->parsed) return !md->bad;
    md->parsed = true;
    md->P = parse_image(md->image, remote_sm());
    if (md->P.load_image.empty()) {
        md->bad = true;
        dbg("module %u unusable remotely: %s", md->mid, md->P.error.c_str());
        return false;
    }
    hash128(md->P.load_image.data(), md->P.load_image.size(), md->hash);
    // layouts are fine even if some ELF section looked weird
    return true;
}

bool ensure_module_sent(Mod* md) {
    State& s = S();
    if (md->sent) return true;
    if (!ensure_parsed(md)) return false;
    bool cached = s.server_has.count({md->hash[0], md->hash[1]}) != 0;
    MsgModule m{md->mid, cached ? 0u : 1u, {md->hash[0], md->hash[1]},
                cached ? 0 : md->P.load_image.size()};
    if (!post(OP_MODULE_LOAD, &m, sizeof m, cached ? nullptr : md->P.load_image.data(),
              cached ? 0 : md->P.load_image.size()))
        return false;
    if (!cached) {
        s.b_up += md->P.load_image.size();
        s.server_has.insert({md->hash[0], md->hash[1]});
    }
    md->sent = true;
    return true;
}

bool ensure_func_sent(Func* f) {
    if (f->sent) return true;
    if (!ensure_module_sent(f->mod)) return false;
    auto it = f->mod->P.kernels.find(f->name);
    f->L = it == f->mod->P.kernels.end() ? nullptr : &it->second;
    MsgFunc m{f->fid, f->mod->mid, uint32_t(f->name.size())};
    if (!post(OP_FUNC_GET, &m, sizeof m, f->name.data(), f->name.size())) return false;
    for (auto& a : f->attrs) {
        MsgFuncAttr fa{f->fid, a.first, a.second};
        post(OP_FUNC_ATTR, &fa, sizeof fa);
    }
    if (f->cache >= 0) {
        MsgFuncAttr fa{f->fid, 0, f->cache};
        post(OP_FUNC_CACHE, &fa, sizeof fa);
    }
    f->sent = true;
    return true;
}

void forget_module(CUmodule m) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    auto it = s.mods.find(m);
    if (it == s.mods.end()) return;
    Mod* md = it->second;
    if (md->sent) post(OP_MODULE_UNLOAD, &md->mid, 4);
    for (auto f = s.funcs.begin(); f != s.funcs.end();) {
        if (f->second->mod == md) {
            delete f->second;
            f = s.funcs.erase(f);
        } else {
            ++f;
        }
    }
    for (auto a = s.alias_of.begin(); a != s.alias_of.end();)
        a = a->first.first == md ? s.alias_of.erase(a) : std::next(a);
    s.mods.erase(it);
    delete md;
}

CUresult r_launch(Func* f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by,
                  unsigned bz, unsigned shmem, CUstream st, void** params, void** extra,
                  bool coop = false) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (!ensure_func_sent(f)) return f->mod->bad ? CUDA_ERROR_NO_BINARY_FOR_GPU : kLinkErr;
    std::vector<uint8_t>& b = s.argbuf;
    b.clear();
    if (extra) {
        void* bp = nullptr;
        size_t* bs = nullptr;
        for (int i = 0; extra[i] != CU_LAUNCH_PARAM_END; i += 2) {
            if (extra[i] == CU_LAUNCH_PARAM_BUFFER_POINTER) bp = extra[i + 1];
            else if (extra[i] == CU_LAUNCH_PARAM_BUFFER_SIZE) bs = static_cast<size_t*>(extra[i + 1]);
            else return CUDA_ERROR_INVALID_VALUE;
        }
        if (!bp || !bs) return CUDA_ERROR_INVALID_VALUE;
        b.assign(static_cast<uint8_t*>(bp), static_cast<uint8_t*>(bp) + *bs);
    } else if (f->L) {
        b.assign(f->L->total, 0);
        for (size_t i = 0; i < f->L->params.size(); i++) {
            if (!params || !params[i]) return CUDA_ERROR_INVALID_VALUE;
            std::memcpy(b.data() + f->L->params[i].first, params[i], f->L->params[i].second);
        }
    } else {
        warnf("no parameter layout for kernel %s", f->name.c_str());
        return CUDA_ERROR_NOT_SUPPORTED;
    }
    // pointers to __device__ vars are local aliases, translate them
    if (s.alias_end > s.alias_base)
        for (size_t o = 0; o + 8 <= b.size(); o += 8) {
            uint64_t v;
            std::memcpy(&v, b.data() + o, 8);
            if (v >= s.alias_base && v < s.alias_end) {
                v = xlate(v);
                std::memcpy(b.data() + o, &v, 8);
            }
        }
    MsgLaunch m{f->fid, coop ? 1u : 0u, {gx, gy, gz}, {bx, by, bz}, shmem, sid_of(st), uint32_t(b.size())};
    s.n_launch++;
    if (coop) {  // slave rebuilds kernelParams from these offsets
        if (!f->L) return CUDA_ERROR_NOT_SUPPORTED;
        uint32_t n = uint32_t(f->L->params.size());
        b.insert(b.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
        for (auto& pr : f->L->params) {
            uint32_t o = pr.first;
            b.insert(b.end(), reinterpret_cast<uint8_t*>(&o), reinterpret_cast<uint8_t*>(&o) + 4);
        }
    }
    return post(OP_LAUNCH, &m, sizeof m, b.data(), b.size()) ? CUDA_SUCCESS : kLinkErr;
}

Func* remote_func(CUfunction f) {
    State& s = S();
    if (s.fd < 0) return nullptr;
    std::lock_guard<std::recursive_mutex> g(s.mu);
    auto it = s.funcs.find(f);
    return it == s.funcs.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- devices

void init_once() {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (s.inited) return;
    s.inited = true;
    g_debug = getenv("C2C_GPU_DEBUG") ? std::atoi(getenv("C2C_GPU_DEBUG")) : 0;
    REAL(cuDeviceGetCount)(&s.nlocal);
    bool remote = connect_remote();
    std::vector<int> all;  // local ordinals, then -1 = remote
    for (int i = 0; i < s.nlocal; i++) all.push_back(i);
    if (remote) all.push_back(-1);
    const char* cvd = getenv("C2C_VISIBLE_DEVICES");
    if (cvd && *cvd) {
        std::string v = cvd;
        size_t p = 0;
        while (p <= v.size()) {
            size_t q = v.find(',', p);
            if (q == std::string::npos) q = v.size();
            std::string t = v.substr(p, q - p);
            char* e = nullptr;
            long k = std::strtol(t.c_str(), &e, 10);
            if (t.empty() || *e || k < 0 || k >= long(all.size())) break;  // real CUDA stops at the first bad entry too
            s.vis.push_back(all[size_t(k)]);
            p = q + 1;
        }
    } else {
        s.vis = all;
    }
    for (size_t i = 0; i < s.vis.size(); i++)
        if (s.vis[i] < 0) s.remote_ord = int(i);
    if (s.remote_ord < 0 && remote) {  // remote GPU not picked, hang up
        s.dead = true;
        s.cv.notify_all();
        close(s.fd);
        s.fd = -1;
    }
    dbg("%d local GPU(s), remote GPU %s, visible ordinals: %zu", s.nlocal,
        s.fd >= 0 ? "attached" : "none", s.vis.size());
}

bool remote_dev(CUdevice d) {
    State& s = S();
    return s.fd >= 0 && d == s.remote_ord;
}
// visible ordinal -> real local one (-1 = invalid or remote)
int real_dev(CUdevice d) {
    State& s = S();
    if (d < 0 || size_t(d) >= s.vis.size()) return -1;
    return s.vis[size_t(d)];
}
CUdevice vis_of_real(int r) {
    State& s = S();
    for (size_t i = 0; i < s.vis.size(); i++)
        if (s.vis[i] == r) return CUdevice(i);
    return CUdevice(r);
}

int rattr(int a) {
    State& s = S();
    switch (a) {  // remote GPU looks like a discrete GPU from here
    case CU_DEVICE_ATTRIBUTE_INTEGRATED:
    case CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY:
    case CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY:
    case CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS:
    case CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS:
    case CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES:
    case CU_DEVICE_ATTRIBUTE_DIRECT_MANAGED_MEM_ACCESS_FROM_HOST:
    case CU_DEVICE_ATTRIBUTE_HOST_NATIVE_ATOMIC_SUPPORTED:
    case CU_DEVICE_ATTRIBUTE_CAN_USE_HOST_POINTER_FOR_REGISTERED_MEM:
    case CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED:
    case CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED:
    case CU_DEVICE_ATTRIBUTE_MEMORY_POOLS_SUPPORTED:
    case CU_DEVICE_ATTRIBUTE_COOPERATIVE_MULTI_DEVICE_LAUNCH:
    case CU_DEVICE_ATTRIBUTE_MULTI_GPU_BOARD:
    case CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED:
        return 0;
    case CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID:
        return 0xc2c;
    case CU_DEVICE_ATTRIBUTE_PCI_BUS_ID:
    case CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID:
        return 0;
    case CU_DEVICE_ATTRIBUTE_MULTI_GPU_BOARD_GROUP_ID:
        return 0xc2c;
    default:
        return a > 0 && a < kMaxAttr ? s.hello.attr[a] : 0;
    }
}

bool setup_window();

// Shadow ctx = a real ctx on the local GPU (same model as the remote one).
// Reserve the shared VA window BEFORE creating it: once there are two ctxs on
// a GPU the driver refuses big VA reservations (error 208).
CUresult make_shadow(CUcontext* out, unsigned flags) {
    State& s = S();
    bool need_win = !s.win_size && !s.win_failed;
    if (need_win) {
        CUcontext cur = current_ctx();
        unsigned pf = 0;
        int active = 0;
        REAL(cuDevicePrimaryCtxGetState)(0, &pf, &active);
        if (cur && !is_shadow(cur)) {
            setup_window();
            need_win = false;
        } else if (active) {
            CUcontext p = nullptr;
            if (REAL(cuDevicePrimaryCtxRetain)(&p, 0) == CUDA_SUCCESS) {
                REAL(cuCtxPushCurrent_v2)(p);
                setup_window();
                REAL(cuCtxPopCurrent_v2)(nullptr);
                REAL(cuDevicePrimaryCtxRelease_v2)(0);
                need_win = false;
            }
        }
    }
    int local0 = 0;
    CUresult r = REAL(cuCtxCreate_v2)(out, flags, local0);
    if (r) return r;
    s.shadow.insert(*out);
    if (need_win) setup_window();  // only ctx so far, reserve while it's current
    return CUDA_SUCCESS;
}

}  // namespace

// ------------------------------------------------------------ entry points

static thread_local unsigned long t_unsup;
static CUresult c2c_unsupported() {
    static std::mutex m;
    static std::set<unsigned long> told;
    std::lock_guard<std::mutex> g(m);
    if (told.insert(t_unsup).second)
        warnf("%s is not supported on the remote GPU", kSymNames[t_unsup]);
    return CUDA_ERROR_NOT_SUPPORTED;
}

// every generated trampoline calls this: where should driver function i go?
extern "C" __attribute__((visibility("default"))) void* c2c_route(unsigned long i) {
    load_real();
    State& s = S();
    if (s.fd < 0 || kSymSafe[i]) return g_real[i];
    bool remote;
    {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        remote = is_shadow(current_ctx());
    }
    if (!remote) return g_real[i];
    t_unsup = i;
    return reinterpret_cast<void*>(&c2c_unsupported);
}

C2CAPI CUresult cuInit(unsigned int flags) {
    load_real();
    if (!g_lib) return CUDA_ERROR_NO_DEVICE;
    // CUDA_VISIBLE_DEVICES includes the remote GPU, so we parse it ourselves
    // and hide it from NVIDIA's driver (it sees all local GPUs)
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = getenv("CUDA_VISIBLE_DEVICES");
        if (v) {
            setenv("C2C_VISIBLE_DEVICES", v, 1);
            unsetenv("CUDA_VISIBLE_DEVICES");
        }
    });
    CUresult r = REAL(cuInit)(flags);
    if (r) return r;
    init_once();
    return CUDA_SUCCESS;
}

C2CAPI CUresult cuDeviceGetCount(int* n) {
    if (!n) return CUDA_ERROR_INVALID_VALUE;
    if (!S().inited) return CUDA_ERROR_NOT_INITIALIZED;
    *n = int(S().vis.size());
    return CUDA_SUCCESS;
}

C2CAPI CUresult cuDeviceGet(CUdevice* d, int ord) {
    if (!d) return CUDA_ERROR_INVALID_VALUE;
    if (!S().inited) return CUDA_ERROR_NOT_INITIALIZED;
    if (ord < 0 || size_t(ord) >= S().vis.size()) return CUDA_ERROR_INVALID_DEVICE;
    *d = ord;
    return CUDA_SUCCESS;
}

C2CAPI CUresult cuDeviceGetName(char* name, int len, CUdevice d) {
    if (remote_dev(d)) {
        if (!name || len <= 0) return CUDA_ERROR_INVALID_VALUE;
        std::snprintf(name, size_t(len), "%s", S().hello.name);
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetName)(name, len, r);
}

C2CAPI CUresult cuDeviceGetUuid(CUuuid* u, CUdevice d) {
    if (remote_dev(d)) {
        if (!u) return CUDA_ERROR_INVALID_VALUE;
        std::memcpy(u->bytes, S().hello.uuid, 16);
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetUuid)(u, r);
}
C2CAPI CUresult cuDeviceGetUuid_v2(CUuuid* u, CUdevice d) {
    if (remote_dev(d)) return cuDeviceGetUuid(u, d);
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetUuid_v2)(u, r);
}

C2CAPI CUresult cuDeviceTotalMem_v2(size_t* b, CUdevice d) {
    if (remote_dev(d)) {
        if (!b) return CUDA_ERROR_INVALID_VALUE;
        *b = S().hello.total_mem;
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceTotalMem_v2)(b, r);
}

C2CAPI CUresult cuDeviceGetAttribute(int* v, CUdevice_attribute a, CUdevice d) {
    if (remote_dev(d)) {
        if (!v) return CUDA_ERROR_INVALID_VALUE;
        *v = rattr(a);
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetAttribute)(v, a, r);
}

C2CAPI CUresult cuDeviceComputeCapability(int* major, int* minor, CUdevice d) {
    if (remote_dev(d)) {
        *major = rattr(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR);
        *minor = rattr(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR);
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceComputeCapability)(major, minor, r);
}

C2CAPI CUresult cuDeviceGetProperties(CUdevprop* p, CUdevice d) {
    if (remote_dev(d)) {
        if (!p) return CUDA_ERROR_INVALID_VALUE;
        p->maxThreadsPerBlock = rattr(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK);
        p->maxThreadsDim[0] = rattr(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X);
        p->maxThreadsDim[1] = rattr(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y);
        p->maxThreadsDim[2] = rattr(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z);
        p->maxGridSize[0] = rattr(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X);
        p->maxGridSize[1] = rattr(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y);
        p->maxGridSize[2] = rattr(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z);
        p->sharedMemPerBlock = rattr(CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK);
        p->totalConstantMemory = rattr(CU_DEVICE_ATTRIBUTE_TOTAL_CONSTANT_MEMORY);
        p->SIMDWidth = rattr(CU_DEVICE_ATTRIBUTE_WARP_SIZE);
        p->memPitch = rattr(CU_DEVICE_ATTRIBUTE_MAX_PITCH);
        p->regsPerBlock = rattr(CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK);
        p->clockRate = rattr(CU_DEVICE_ATTRIBUTE_CLOCK_RATE);
        p->textureAlign = rattr(CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT);
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetProperties)(p, r);
}

C2CAPI CUresult cuDeviceGetPCIBusId(char* id, int len, CUdevice d) {
    if (remote_dev(d)) {
        if (!id || len <= 0) return CUDA_ERROR_INVALID_VALUE;
        std::snprintf(id, size_t(len), "%04x:%02x:%02x.0", rattr(CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID),
                      rattr(CU_DEVICE_ATTRIBUTE_PCI_BUS_ID), rattr(CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID));
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDeviceGetPCIBusId)(id, len, r);
}

C2CAPI CUresult cuDeviceGetByPCIBusId(CUdevice* d, const char* id) {
    State& s = S();
    if (!d || !id) return CUDA_ERROR_INVALID_VALUE;
    if (s.fd >= 0 && s.remote_ord >= 0) {
        char mine[32];
        cuDeviceGetPCIBusId(mine, sizeof mine, s.remote_ord);
        if (!strcasecmp(mine, id)) {
            *d = s.remote_ord;
            return CUDA_SUCCESS;
        }
    }
    CUdevice r;
    CUresult rc = REAL(cuDeviceGetByPCIBusId)(&r, id);
    if (!rc) *d = vis_of_real(r);
    return rc;
}

C2CAPI CUresult cuDeviceCanAccessPeer(int* can, CUdevice a, CUdevice b) {
    if (!can) return CUDA_ERROR_INVALID_VALUE;
    if (remote_dev(a) || remote_dev(b)) {
        *can = 0;  // no peer access over the link, copies get staged
        return CUDA_SUCCESS;
    }
    int ra = real_dev(a), rb = real_dev(b);
    if (ra < 0 || rb < 0) return CUDA_ERROR_INVALID_DEVICE;
    return REAL(cuDeviceCanAccessPeer)(can, ra, rb);
}

C2CAPI CUresult cuDeviceGetP2PAttribute(int* v, CUdevice_P2PAttribute a, CUdevice x, CUdevice y) {
    if (!v) return CUDA_ERROR_INVALID_VALUE;
    if (remote_dev(x) || remote_dev(y)) {
        *v = 0;
        return CUDA_SUCCESS;
    }
    int rx = real_dev(x), ry = real_dev(y);
    if (rx < 0 || ry < 0) return CUDA_ERROR_INVALID_DEVICE;
    return REAL(cuDeviceGetP2PAttribute)(v, a, rx, ry);
}

// --- contexts

C2CAPI CUresult cuDevicePrimaryCtxRetain(CUcontext* c, CUdevice d) {
    State& s = S();
    if (remote_dev(d)) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        if (!s.prim) {
            CUresult r = make_shadow(&s.prim, s.prim_flags);
            if (r) return r;
            REAL(cuCtxPopCurrent_v2)(nullptr);
        }
        s.prim_ref++;
        *c = s.prim;
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDevicePrimaryCtxRetain)(c, r);
}

static CUresult prim_release(CUdevice d) {
    State& s = S();
    if (remote_dev(d)) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        if (s.prim_ref > 0) s.prim_ref--;
        return CUDA_SUCCESS;  // shadow ctx sticks around until reset/exit
    }
    int r = real_dev(d);
    if (r < 0) return CUDA_ERROR_INVALID_DEVICE;
    return REAL(cuDevicePrimaryCtxRelease_v2)(r);
}
C2CAPI CUresult cuDevicePrimaryCtxRelease_v2(CUdevice d) { return prim_release(d); }

C2CAPI CUresult cuDevicePrimaryCtxGetState(CUdevice d, unsigned* flags, int* active) {
    State& s = S();
    if (remote_dev(d)) {
        if (flags) *flags = s.prim_flags;
        if (active) *active = s.prim && s.prim_ref > 0;
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuDevicePrimaryCtxGetState)(r, flags, active);
}

static CUresult prim_setflags(CUdevice d, unsigned f) {
    if (remote_dev(d)) {
        S().prim_flags = f;
        return CUDA_SUCCESS;
    }
    int r = real_dev(d);
    if (r < 0) return CUDA_ERROR_INVALID_DEVICE;
    return REAL(cuDevicePrimaryCtxSetFlags_v2)(r, f);
}
C2CAPI CUresult cuDevicePrimaryCtxSetFlags_v2(CUdevice d, unsigned f) { return prim_setflags(d, f); }

static CUresult prim_reset(CUdevice d) {
    State& s = S();
    if (remote_dev(d)) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        CUresult r = call(OP_RESET, nullptr, 0);
        // remote side is wiped, forget everything we tracked
        for (auto& kv : s.ranges)
            if (kv.second.kind == Kind::Alloc) win_give(kv.first, kv.second.reserved);
        for (auto& kv : s.slabs) win_give(kv.first, kGran);
        s.ranges.clear();
        s.slabs.clear();
        s.cur_slab = 0;
        s.streams.clear();
        s.events.clear();
        for (auto& kv : s.funcs) delete kv.second;
        s.funcs.clear();
        for (auto& kv : s.mods) delete kv.second;
        s.mods.clear();
        s.alias_of.clear();
        s.alias_next = s.alias_base;
        if (s.prim) {
            s.shadow.erase(s.prim);
            REAL(cuCtxDestroy_v2)(s.prim);
            s.prim = nullptr;
        }
        s.prim_ref = 0;
        return r;
    }
    int r = real_dev(d);
    if (r < 0) return CUDA_ERROR_INVALID_DEVICE;
    return REAL(cuDevicePrimaryCtxReset_v2)(r);
}
C2CAPI CUresult cuDevicePrimaryCtxReset_v2(CUdevice d) { return prim_reset(d); }

C2CAPI CUresult cuCtxCreate_v2(CUcontext* c, unsigned flags, CUdevice d) {
    if (remote_dev(d)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        return make_shadow(c, flags);
    }
    int r = real_dev(d);
    return r < 0 ? CUDA_ERROR_INVALID_DEVICE : REAL(cuCtxCreate_v2)(c, flags, r);
}

C2CAPI CUresult cuCtxDestroy_v2(CUcontext c) {
    State& s = S();
    {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        if (c == s.prim) return CUDA_ERROR_INVALID_CONTEXT;  // primary ctx: Release/Reset instead
        s.shadow.erase(c);
    }
    return REAL(cuCtxDestroy_v2)(c);
}

C2CAPI CUresult cuCtxGetDevice(CUdevice* d) {
    State& s = S();
    if (!d) return CUDA_ERROR_INVALID_VALUE;
    if (on_remote()) {
        *d = s.remote_ord;
        return CUDA_SUCCESS;
    }
    CUdevice r;
    CUresult rc = REAL(cuCtxGetDevice)(&r);
    if (!rc) *d = vis_of_real(r);
    return rc;
}

C2CAPI CUresult cuCtxSynchronize() {
    if (on_remote()) {
        State& s = S();
        std::lock_guard<std::recursive_mutex> g(s.mu);
        CUresult r = call(OP_CTX_SYNC, nullptr, 0);
        if (r == CUDA_SUCCESS) {
            s.clean_seq = s.post_seq;
            s.clean_all = true;
        }
        return r;
    }
    return REAL(cuCtxSynchronize)();
}

C2CAPI CUresult cuCtxSetLimit(CUlimit l, size_t v) {
    CUresult r = REAL(cuCtxSetLimit)(l, v);
    if (on_remote()) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        MsgLimit m{int32_t(l), v};
        post(OP_CTX_LIMIT, &m, sizeof m);
    }
    return r;
}

C2CAPI CUresult cuCtxSetCacheConfig(CUfunc_cache c) {
    CUresult r = REAL(cuCtxSetCacheConfig)(c);
    if (on_remote()) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        int32_t v = c;
        post(OP_CTX_CACHE, &v, 4);
    }
    return r;
}

C2CAPI CUresult cuCtxEnablePeerAccess(CUcontext peer, unsigned flags) {
    State& s = S();
    if (is_shadow(peer) || on_remote()) return CUDA_ERROR_PEER_ACCESS_UNSUPPORTED;
    (void)s;
    return REAL(cuCtxEnablePeerAccess)(peer, flags);
}

// --- memory

C2CAPI CUresult cuMemAlloc_v2(CUdeviceptr* p, size_t n) {
    if (on_remote()) return r_alloc(p, n);
    return REAL(cuMemAlloc_v2)(p, n);
}

C2CAPI CUresult cuMemAllocPitch_v2(CUdeviceptr* p, size_t* pitch, size_t w, size_t h, unsigned elem) {
    if (on_remote()) {
        if (!pitch || !w || !h || (elem != 4 && elem != 8 && elem != 16)) return CUDA_ERROR_INVALID_VALUE;
        *pitch = round_up(w, size_t(rattr(CU_DEVICE_ATTRIBUTE_TEXTURE_PITCH_ALIGNMENT)) ?: 512);
        return r_alloc(p, *pitch * h);
    }
    return REAL(cuMemAllocPitch_v2)(p, pitch, w, h, elem);
}

C2CAPI CUresult cuMemFree_v2(CUdeviceptr p) {
    if (is_remote_ptr(p)) return r_free(p);
    if (p == 0) return CUDA_SUCCESS;
    return REAL(cuMemFree_v2)(p);
}

C2CAPI CUresult cuMemGetInfo_v2(size_t* fr, size_t* tot) {
    if (on_remote()) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        uint64_t v[2] = {0, 0};
        CUresult r = call(OP_MEM_INFO, nullptr, 0, v, sizeof v);
        if (fr) *fr = v[0];
        if (tot) *tot = v[1];
        return r;
    }
    return REAL(cuMemGetInfo_v2)(fr, tot);
}

C2CAPI CUresult cuMemGetAddressRange_v2(CUdeviceptr* base, size_t* size, CUdeviceptr p) {
    State& s = S();
    if (is_remote_ptr(p)) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        uint64_t b;
        const Range* r = find_range(p, &b);
        if (base) *base = b;
        if (size) *size = r->size;
        return CUDA_SUCCESS;
    }
    return REAL(cuMemGetAddressRange_v2)(base, size, p);
}

C2CAPI CUresult cuMemHostGetDevicePointer_v2(CUdeviceptr* d, void* h, unsigned flags) {
    if (on_remote()) return CUDA_ERROR_NOT_SUPPORTED;  // can't do zero-copy across boards
    return REAL(cuMemHostGetDevicePointer_v2)(d, h, flags);
}

static CUresult pattr(void* data, CUpointer_attribute a, CUdeviceptr p) {
    State& s = S();
    uint64_t b;
    const Range* r = find_range(p, &b);
    switch (a) {
    case CU_POINTER_ATTRIBUTE_CONTEXT:
        if (!s.prim) cuDevicePrimaryCtxRetain(&s.prim, s.remote_ord);
        *static_cast<CUcontext*>(data) = s.prim;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_MEMORY_TYPE:
        *static_cast<unsigned*>(data) = CU_MEMORYTYPE_DEVICE;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_DEVICE_POINTER:
        *static_cast<CUdeviceptr*>(data) = p;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_HOST_POINTER:
        return CUDA_ERROR_INVALID_VALUE;
    case CU_POINTER_ATTRIBUTE_SYNC_MEMOPS:
    case CU_POINTER_ATTRIBUTE_MAPPED:
        *static_cast<int*>(data) = 1;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_BUFFER_ID:
        *static_cast<unsigned long long*>(data) = b;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_IS_MANAGED:
    case CU_POINTER_ATTRIBUTE_IS_LEGACY_CUDA_IPC_CAPABLE:
        *static_cast<int*>(data) = 0;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL:
        *static_cast<int*>(data) = s.remote_ord;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_RANGE_START_ADDR:
        *static_cast<CUdeviceptr*>(data) = b;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_RANGE_SIZE:
        *static_cast<size_t*>(data) = r->size;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_ALLOWED_HANDLE_TYPES:
        *static_cast<unsigned long long*>(data) = 0;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_ACCESS_FLAGS:
        *static_cast<unsigned*>(data) = CU_POINTER_ATTRIBUTE_ACCESS_FLAG_READWRITE;
        return CUDA_SUCCESS;
    case CU_POINTER_ATTRIBUTE_MEMPOOL_HANDLE:
        *static_cast<void**>(data) = nullptr;
        return CUDA_SUCCESS;
    default:
        return CUDA_ERROR_INVALID_VALUE;
    }
}

C2CAPI CUresult cuPointerGetAttribute(void* data, CUpointer_attribute a, CUdeviceptr p) {
    if (is_remote_ptr(p)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        return data ? pattr(data, a, p) : CUDA_ERROR_INVALID_VALUE;
    }
    return REAL(cuPointerGetAttribute)(data, a, p);
}

C2CAPI CUresult cuPointerGetAttributes(unsigned n, CUpointer_attribute* a, void** data, CUdeviceptr p) {
    if (is_remote_ptr(p)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        for (unsigned i = 0; i < n; i++)
            if (data[i] && pattr(data[i], a[i], p) != CUDA_SUCCESS) {
                // unknown attrs read as 0, same as the driver
                if (a[i] == CU_POINTER_ATTRIBUTE_HOST_POINTER) *static_cast<void**>(data[i]) = nullptr;
            }
        return CUDA_SUCCESS;
    }
    return REAL(cuPointerGetAttributes)(n, a, data, p);
}

// --- copies

#define STREAM_ARG(st) (is_remote_stream(st) ? sid_of(st) : kStreamLegacy)

C2CAPI CUresult cuMemcpyHtoD_v2(CUdeviceptr d, const void* h, size_t n) {
    if (is_remote_ptr(d)) return r_htod(d, h, n, kStreamLegacy);
    return REAL(cuMemcpyHtoD_v2)(d, h, n);
}
C2CAPI CUresult cuMemcpyDtoH_v2(void* h, CUdeviceptr d, size_t n) {
    if (is_remote_ptr(d)) return r_dtoh(h, d, n, kStreamLegacy);
    return REAL(cuMemcpyDtoH_v2)(h, d, n);
}
C2CAPI CUresult cuMemcpyDtoD_v2(CUdeviceptr d, CUdeviceptr s, size_t n) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, nullptr, false);
    return REAL(cuMemcpyDtoD_v2)(d, s, n);
}
C2CAPI CUresult cuMemcpy(CUdeviceptr d, CUdeviceptr s, size_t n) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, nullptr, false);
    return REAL(cuMemcpy)(d, s, n);
}
C2CAPI CUresult cuMemcpyAsync(CUdeviceptr d, CUdeviceptr s, size_t n, CUstream st) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, st, true);
    return REAL(cuMemcpyAsync)(d, s, n, st);
}
C2CAPI CUresult cuMemcpyHtoDAsync_v2(CUdeviceptr d, const void* h, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_htod(d, h, n, STREAM_ARG(st));
    return REAL(cuMemcpyHtoDAsync_v2)(d, h, n, st);
}
C2CAPI CUresult cuMemcpyDtoHAsync_v2(void* h, CUdeviceptr d, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_dtoh_async(h, d, n, STREAM_ARG(st));
    return REAL(cuMemcpyDtoHAsync_v2)(h, d, n, st);
}
C2CAPI CUresult cuMemcpyDtoDAsync_v2(CUdeviceptr d, CUdeviceptr s, size_t n, CUstream st) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, st, true);
    return REAL(cuMemcpyDtoDAsync_v2)(d, s, n, st);
}
C2CAPI CUresult cuMemcpyPeer(CUdeviceptr d, CUcontext dc, CUdeviceptr s, CUcontext sc, size_t n) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, nullptr, false);
    return REAL(cuMemcpyPeer)(d, dc, s, sc, n);
}
C2CAPI CUresult cuMemcpyPeerAsync(CUdeviceptr d, CUcontext dc, CUdeviceptr s, CUcontext sc, size_t n,
                                  CUstream st) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, st, true);
    return REAL(cuMemcpyPeerAsync)(d, dc, s, sc, n, st);
}

// --- arrays and 2D/3D copies

static bool is_remote_array(CUarray a) {
    return (reinterpret_cast<uint64_t>(a) & 0xFFFF000000000000ull) == kArrayTag;
}
static std::mutex g_arr_mu;
static std::unordered_map<uint64_t, CUDA_ARRAY3D_DESCRIPTOR> g_arrays;  // arrays living on the slave

static CUresult array_create(CUarray* a, const CUDA_ARRAY3D_DESCRIPTOR* d) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    uint64_t id = kArrayTag | s.next_id++;
    MsgArray m{id, *d};
    CUresult r = call(OP_ARRAY_CREATE, &m, sizeof m);
    if (r) return r;
    {
        std::lock_guard<std::mutex> ga(g_arr_mu);
        g_arrays[id] = *d;
    }
    *a = reinterpret_cast<CUarray>(id);
    return CUDA_SUCCESS;
}

C2CAPI CUresult cuArray3DCreate_v2(CUarray* a, const CUDA_ARRAY3D_DESCRIPTOR* d) {
    if (on_remote()) return a && d ? array_create(a, d) : CUDA_ERROR_INVALID_VALUE;
    return REAL(cuArray3DCreate_v2)(a, d);
}
C2CAPI CUresult cuArrayCreate_v2(CUarray* a, const CUDA_ARRAY_DESCRIPTOR* d) {
    if (on_remote()) {
        if (!a || !d) return CUDA_ERROR_INVALID_VALUE;
        CUDA_ARRAY3D_DESCRIPTOR d3{d->Width, d->Height, 0, d->Format, d->NumChannels, 0};
        return array_create(a, &d3);
    }
    return REAL(cuArrayCreate_v2)(a, d);
}
C2CAPI CUresult cuArrayDestroy(CUarray a) {
    if (is_remote_array(a)) {
        uint64_t id = reinterpret_cast<uint64_t>(a);
        {
            std::lock_guard<std::mutex> ga(g_arr_mu);
            if (!g_arrays.erase(id)) return CUDA_ERROR_INVALID_HANDLE;
        }
        std::lock_guard<std::recursive_mutex> g(S().mu);
        return post(OP_ARRAY_DESTROY, &id, 8) ? CUDA_SUCCESS : kLinkErr;
    }
    return REAL(cuArrayDestroy)(a);
}
C2CAPI CUresult cuArray3DGetDescriptor_v2(CUDA_ARRAY3D_DESCRIPTOR* d, CUarray a) {
    if (is_remote_array(a)) {
        std::lock_guard<std::mutex> ga(g_arr_mu);
        auto it = g_arrays.find(reinterpret_cast<uint64_t>(a));
        if (it == g_arrays.end() || !d) return CUDA_ERROR_INVALID_HANDLE;
        *d = it->second;
        return CUDA_SUCCESS;
    }
    return REAL(cuArray3DGetDescriptor_v2)(d, a);
}
C2CAPI CUresult cuArrayGetDescriptor_v2(CUDA_ARRAY_DESCRIPTOR* d, CUarray a) {
    if (is_remote_array(a)) {
        CUDA_ARRAY3D_DESCRIPTOR d3;
        CUresult r = cuArray3DGetDescriptor_v2(&d3, a);
        if (r) return r;
        *d = CUDA_ARRAY_DESCRIPTOR{d3.Width, d3.Height, d3.Format, d3.NumChannels};
        return CUDA_SUCCESS;
    }
    return REAL(cuArrayGetDescriptor_v2)(d, a);
}

namespace {
enum class Side { Remote, LocalDev, LocalArray, Host };
Side side_of(CUmemorytype t, CUdeviceptr dev, CUarray arr) {
    switch (t) {
    case CU_MEMORYTYPE_HOST: return Side::Host;
    case CU_MEMORYTYPE_ARRAY: return is_remote_array(arr) ? Side::Remote : Side::LocalArray;
    default: {
        Where w = where(dev);
        return w == Where::Remote ? Side::Remote : w == Where::LocalDev ? Side::LocalDev : Side::Host;
    }
    }
}
}  // namespace

// 3D copy where at least one side is on the remote GPU
static CUresult remote_copy3d(CUDA_MEMCPY3D d, CUstream st) {
    size_t W = d.WidthInBytes, H = d.Height ? d.Height : 1, D = d.Depth ? d.Depth : 1;
    if (!d.srcPitch) d.srcPitch = W;
    if (!d.dstPitch) d.dstPitch = W;
    if (!d.srcHeight) d.srcHeight = H;
    if (!d.dstHeight) d.dstHeight = H;
    Side ss = side_of(d.srcMemoryType, d.srcDevice, d.srcArray);
    Side ds = side_of(d.dstMemoryType, d.dstDevice, d.dstArray);
    std::vector<uint8_t> packed;
    // host addr of row y, slice z of a host operand
    auto host_row = [](const void* base, size_t x, size_t y, size_t z, size_t pitch, size_t height,
                       size_t row, size_t slice) {
        return static_cast<const uint8_t*>(base) + (z + slice) * height * pitch + (y + row) * pitch + x;
    };
    auto local_to_packed = [&](CUDA_MEMCPY3D src) -> CUresult {  // local GPU/array -> tightly packed host buf
        packed.resize(W * H * D);
        src.dstMemoryType = CU_MEMORYTYPE_HOST;
        src.dstHost = packed.data();
        src.dstXInBytes = src.dstY = src.dstZ = 0;
        src.dstPitch = W;
        src.dstHeight = H;
        return REAL(cuMemcpy3D_v2)(&src);
    };
    MsgCopy3D m{};
    m.d = d;
    m.stream = is_remote_stream(st) ? sid_of(st) : kStreamLegacy;
    // source
    if (ss == Side::Host) {
        const void* base = d.srcMemoryType == CU_MEMORYTYPE_HOST ? d.srcHost
                                                                 : reinterpret_cast<const void*>(d.srcDevice);
        packed.resize(W * H * D);
        for (size_t z = 0; z < D; z++)
            for (size_t y = 0; y < H; y++)
                std::memcpy(packed.data() + (z * H + y) * W,
                            host_row(base, d.srcXInBytes, d.srcY, d.srcZ, d.srcPitch, d.srcHeight, y, z), W);
        m.src_host = 1;
    } else if (ss == Side::LocalDev || ss == Side::LocalArray) {
        CUresult r = local_to_packed(d);
        if (r) return r;
        m.src_host = 1;
    } else if (d.srcMemoryType == CU_MEMORYTYPE_ARRAY) {
        m.d.srcArray = d.srcArray;  // tagged id, the slave looks it up
    } else {
        m.d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        m.d.srcDevice = xlate(d.srcDevice);
    }
    if (m.src_host) {
        m.d.srcHost = nullptr;
        m.packed = packed.size();
    }
    // destination
    bool dst_local = ds == Side::LocalDev || ds == Side::LocalArray;
    if (ds == Side::Host || dst_local) {
        m.dst_host = 1;
    } else if (d.dstMemoryType != CU_MEMORYTYPE_ARRAY) {
        m.d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        m.d.dstDevice = xlate(d.dstDevice);
    }
    if (m.dst_host) m.d.dstHost = nullptr;
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (!m.dst_host) {
        s.b_up += packed.size();
        return post(OP_COPY3D, &m, sizeof m, packed.data(), packed.size()) ? CUDA_SUCCESS : kLinkErr;
    }
    std::vector<uint8_t> back(W * H * D);
    CUresult r = call(OP_COPY3D, &m, sizeof m, back.data(), back.size(), packed.data(), packed.size());
    if (r) return r;
    s.b_down += back.size();
    if (dst_local) {  // packed host buf -> local GPU mem/array
        CUDA_MEMCPY3D l = d;
        l.srcMemoryType = CU_MEMORYTYPE_HOST;
        l.srcHost = back.data();
        l.srcXInBytes = l.srcY = l.srcZ = 0;
        l.srcPitch = W;
        l.srcHeight = H;
        return REAL(cuMemcpy3D_v2)(&l);
    }
    void* base = d.dstMemoryType == CU_MEMORYTYPE_HOST ? d.dstHost : reinterpret_cast<void*>(d.dstDevice);
    for (size_t z = 0; z < D; z++)
        for (size_t y = 0; y < H; y++)
            std::memcpy(const_cast<uint8_t*>(host_row(base, d.dstXInBytes, d.dstY, d.dstZ, d.dstPitch,
                                                      d.dstHeight, y, z)),
                        back.data() + (z * H + y) * W, W);
    return CUDA_SUCCESS;
}

static bool involves_remote(CUmemorytype st, CUdeviceptr sd, CUarray sa, CUmemorytype dt, CUdeviceptr dd,
                            CUarray da) {
    auto rem = [](CUmemorytype t, CUdeviceptr p, CUarray a) {
        if (t == CU_MEMORYTYPE_ARRAY) return is_remote_array(a);
        if (t == CU_MEMORYTYPE_HOST) return false;
        return is_remote_ptr(p);
    };
    return rem(st, sd, sa) || rem(dt, dd, da);
}

static CUDA_MEMCPY3D to3d(const CUDA_MEMCPY2D* c) {
    CUDA_MEMCPY3D d{};
    d.srcXInBytes = c->srcXInBytes;
    d.srcY = c->srcY;
    d.srcMemoryType = c->srcMemoryType;
    d.srcHost = c->srcHost;
    d.srcDevice = c->srcDevice;
    d.srcArray = c->srcArray;
    d.srcPitch = c->srcPitch;
    d.dstXInBytes = c->dstXInBytes;
    d.dstY = c->dstY;
    d.dstMemoryType = c->dstMemoryType;
    d.dstHost = c->dstHost;
    d.dstDevice = c->dstDevice;
    d.dstArray = c->dstArray;
    d.dstPitch = c->dstPitch;
    d.WidthInBytes = c->WidthInBytes;
    d.Height = c->Height;
    d.Depth = 1;
    return d;
}
static bool remote2d(const CUDA_MEMCPY2D* c) {
    return c && involves_remote(c->srcMemoryType, c->srcDevice, c->srcArray, c->dstMemoryType, c->dstDevice,
                                c->dstArray);
}
static bool remote3d(const CUDA_MEMCPY3D* c) {
    return c && involves_remote(c->srcMemoryType, c->srcDevice, c->srcArray, c->dstMemoryType, c->dstDevice,
                                c->dstArray);
}
C2CAPI CUresult cuMemcpy2D_v2(const CUDA_MEMCPY2D* c) {
    return remote2d(c) ? remote_copy3d(to3d(c), nullptr) : REAL(cuMemcpy2D_v2)(c);
}
C2CAPI CUresult cuMemcpy2DUnaligned_v2(const CUDA_MEMCPY2D* c) {
    return remote2d(c) ? remote_copy3d(to3d(c), nullptr) : REAL(cuMemcpy2DUnaligned_v2)(c);
}
C2CAPI CUresult cuMemcpy2DAsync_v2(const CUDA_MEMCPY2D* c, CUstream st) {
    return remote2d(c) ? remote_copy3d(to3d(c), st) : REAL(cuMemcpy2DAsync_v2)(c, st);
}
C2CAPI CUresult cuMemcpy3D_v2(const CUDA_MEMCPY3D* c) {
    return remote3d(c) ? remote_copy3d(*c, nullptr) : REAL(cuMemcpy3D_v2)(c);
}
C2CAPI CUresult cuMemcpy3DAsync_v2(const CUDA_MEMCPY3D* c, CUstream st) {
    return remote3d(c) ? remote_copy3d(*c, st) : REAL(cuMemcpy3DAsync_v2)(c, st);
}

// 1D array copies are just 3D copies with one row
static CUDA_MEMCPY3D row_copy(size_t n) {
    CUDA_MEMCPY3D d{};
    d.WidthInBytes = n;
    d.Height = 1;
    d.Depth = 1;
    return d;
}
C2CAPI CUresult cuMemcpyHtoA_v2(CUarray a, size_t off, const void* h, size_t n) {
    if (!is_remote_array(a)) return REAL(cuMemcpyHtoA_v2)(a, off, h, n);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_HOST; d.srcHost = h;
    d.dstMemoryType = CU_MEMORYTYPE_ARRAY; d.dstArray = a; d.dstXInBytes = off;
    return remote_copy3d(d, nullptr);
}
C2CAPI CUresult cuMemcpyHtoAAsync_v2(CUarray a, size_t off, const void* h, size_t n, CUstream st) {
    if (!is_remote_array(a)) return REAL(cuMemcpyHtoAAsync_v2)(a, off, h, n, st);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_HOST; d.srcHost = h;
    d.dstMemoryType = CU_MEMORYTYPE_ARRAY; d.dstArray = a; d.dstXInBytes = off;
    return remote_copy3d(d, st);
}
C2CAPI CUresult cuMemcpyAtoH_v2(void* h, CUarray a, size_t off, size_t n) {
    if (!is_remote_array(a)) return REAL(cuMemcpyAtoH_v2)(h, a, off, n);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_ARRAY; d.srcArray = a; d.srcXInBytes = off;
    d.dstMemoryType = CU_MEMORYTYPE_HOST; d.dstHost = h;
    return remote_copy3d(d, nullptr);
}
C2CAPI CUresult cuMemcpyAtoHAsync_v2(void* h, CUarray a, size_t off, size_t n, CUstream st) {
    if (!is_remote_array(a)) return REAL(cuMemcpyAtoHAsync_v2)(h, a, off, n, st);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_ARRAY; d.srcArray = a; d.srcXInBytes = off;
    d.dstMemoryType = CU_MEMORYTYPE_HOST; d.dstHost = h;
    return remote_copy3d(d, st);
}
C2CAPI CUresult cuMemcpyDtoA_v2(CUarray a, size_t off, CUdeviceptr p, size_t n) {
    if (!is_remote_array(a) && !is_remote_ptr(p)) return REAL(cuMemcpyDtoA_v2)(a, off, p, n);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_DEVICE; d.srcDevice = p;
    d.dstMemoryType = CU_MEMORYTYPE_ARRAY; d.dstArray = a; d.dstXInBytes = off;
    return remote_copy3d(d, nullptr);
}
C2CAPI CUresult cuMemcpyAtoD_v2(CUdeviceptr p, CUarray a, size_t off, size_t n) {
    if (!is_remote_array(a) && !is_remote_ptr(p)) return REAL(cuMemcpyAtoD_v2)(p, a, off, n);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_ARRAY; d.srcArray = a; d.srcXInBytes = off;
    d.dstMemoryType = CU_MEMORYTYPE_DEVICE; d.dstDevice = p;
    return remote_copy3d(d, nullptr);
}
C2CAPI CUresult cuMemcpyAtoA_v2(CUarray da, size_t doff, CUarray sa, size_t soff, size_t n) {
    if (!is_remote_array(da) && !is_remote_array(sa)) return REAL(cuMemcpyAtoA_v2)(da, doff, sa, soff, n);
    CUDA_MEMCPY3D d = row_copy(n);
    d.srcMemoryType = CU_MEMORYTYPE_ARRAY; d.srcArray = sa; d.srcXInBytes = soff;
    d.dstMemoryType = CU_MEMORYTYPE_ARRAY; d.dstArray = da; d.dstXInBytes = doff;
    return remote_copy3d(d, nullptr);
}

// --- texture and surface objects

static std::unordered_map<uint64_t, MsgTex> g_texs;          // texture objects on the slave
static std::unordered_map<uint64_t, CUDA_RESOURCE_DESC> g_surfs;

static bool res_is_remote(const CUDA_RESOURCE_DESC* r) {
    switch (r->resType) {
    case CU_RESOURCE_TYPE_ARRAY: return is_remote_array(r->res.array.hArray);
    case CU_RESOURCE_TYPE_LINEAR: return is_remote_ptr(r->res.linear.devPtr);
    case CU_RESOURCE_TYPE_PITCH2D: return is_remote_ptr(r->res.pitch2D.devPtr);
    default: return false;
    }
}
static CUDA_RESOURCE_DESC res_for_remote(const CUDA_RESOURCE_DESC* r) {
    CUDA_RESOURCE_DESC o = *r;
    if (o.resType == CU_RESOURCE_TYPE_LINEAR) o.res.linear.devPtr = xlate(o.res.linear.devPtr);
    if (o.resType == CU_RESOURCE_TYPE_PITCH2D) o.res.pitch2D.devPtr = xlate(o.res.pitch2D.devPtr);
    return o;
}

C2CAPI CUresult cuTexObjectCreate(CUtexObject* t, const CUDA_RESOURCE_DESC* r, const CUDA_TEXTURE_DESC* td,
                                  const CUDA_RESOURCE_VIEW_DESC* v) {
    if (t && r && td && (res_is_remote(r) || on_remote())) {
        if (r->resType == CU_RESOURCE_TYPE_MIPMAPPED_ARRAY) return CUDA_ERROR_NOT_SUPPORTED;
        State& s = S();
        std::lock_guard<std::recursive_mutex> g(s.mu);
        MsgTex m{};
        m.res = res_for_remote(r);
        m.tex = *td;
        m.has_view = v != nullptr;
        if (v) m.view = *v;
        uint64_t obj = 0;
        CUresult rc = call(OP_TEX_CREATE, &m, sizeof m, &obj, 8);
        if (rc) return rc;
        m.res = *r;  // keep the app's version around for the getters
        g_texs[obj] = m;
        *t = obj;
        return CUDA_SUCCESS;
    }
    return REAL(cuTexObjectCreate)(t, r, td, v);
}
// Tex object handles are small ints on both GPUs, so the same value can exist
// on each. Send it remote if we made it there AND (current ctx is remote OR
// the local driver has never heard of it).
static bool remote_tex(CUtexObject t) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (!g_texs.count(t)) return false;
    if (on_remote()) return true;
    CUDA_RESOURCE_DESC d;
    return REAL(cuTexObjectGetResourceDesc)(&d, t) != CUDA_SUCCESS;
}
C2CAPI CUresult cuTexObjectDestroy(CUtexObject t) {
    if (remote_tex(t)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        g_texs.erase(t);
        uint64_t v = t;
        return post(OP_TEX_DESTROY, &v, 8) ? CUDA_SUCCESS : kLinkErr;
    }
    return REAL(cuTexObjectDestroy)(t);
}
C2CAPI CUresult cuTexObjectGetResourceDesc(CUDA_RESOURCE_DESC* d, CUtexObject t) {
    if (remote_tex(t)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        *d = g_texs[t].res;
        return CUDA_SUCCESS;
    }
    return REAL(cuTexObjectGetResourceDesc)(d, t);
}
C2CAPI CUresult cuTexObjectGetTextureDesc(CUDA_TEXTURE_DESC* d, CUtexObject t) {
    if (remote_tex(t)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        *d = g_texs[t].tex;
        return CUDA_SUCCESS;
    }
    return REAL(cuTexObjectGetTextureDesc)(d, t);
}
C2CAPI CUresult cuSurfObjectCreate(CUsurfObject* o, const CUDA_RESOURCE_DESC* r) {
    if (o && r && (res_is_remote(r) || on_remote())) {
        State& s = S();
        std::lock_guard<std::recursive_mutex> g(s.mu);
        CUDA_RESOURCE_DESC m = res_for_remote(r);
        uint64_t obj = 0;
        CUresult rc = call(OP_SURF_CREATE, &m, sizeof m, &obj, 8);
        if (rc) return rc;
        g_surfs[obj] = *r;
        *o = obj;
        return CUDA_SUCCESS;
    }
    return REAL(cuSurfObjectCreate)(o, r);
}
C2CAPI CUresult cuSurfObjectDestroy(CUsurfObject o) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    if (g_surfs.count(o) && (on_remote() || [&] {
            CUDA_RESOURCE_DESC d;
            return REAL(cuSurfObjectGetResourceDesc)(&d, o) != CUDA_SUCCESS;
        }())) {
        g_surfs.erase(o);
        uint64_t v = o;
        return post(OP_SURF_DESTROY, &v, 8) ? CUDA_SUCCESS : kLinkErr;
    }
    return REAL(cuSurfObjectDestroy)(o);
}

// --- memset

#define MEMSET1(name, T, elem)                                                   \
    C2CAPI CUresult name(CUdeviceptr d, T v, size_t n) {                         \
        if (is_remote_ptr(d)) return r_memset(d, 0, v, elem, n, 1, kStreamLegacy); \
        return REAL(name)(d, v, n);                                              \
    }
#define MEMSET1A(name, T, elem)                                                    \
    C2CAPI CUresult name(CUdeviceptr d, T v, size_t n, CUstream st) {              \
        if (is_remote_ptr(d)) return r_memset(d, 0, v, elem, n, 1, STREAM_ARG(st)); \
        return REAL(name)(d, v, n, st);                                            \
    }
#define MEMSET2(name, T, elem)                                                        \
    C2CAPI CUresult name(CUdeviceptr d, size_t p, T v, size_t w, size_t h) {          \
        if (is_remote_ptr(d)) return r_memset(d, p, v, elem, w, h, kStreamLegacy);     \
        return REAL(name)(d, p, v, w, h);                                             \
    }
#define MEMSET2A(name, T, elem)                                                          \
    C2CAPI CUresult name(CUdeviceptr d, size_t p, T v, size_t w, size_t h, CUstream st) { \
        if (is_remote_ptr(d)) return r_memset(d, p, v, elem, w, h, STREAM_ARG(st));       \
        return REAL(name)(d, p, v, w, h, st);                                            \
    }
MEMSET1(cuMemsetD8_v2, unsigned char, 1)
MEMSET1(cuMemsetD16_v2, unsigned short, 2)
MEMSET1(cuMemsetD32_v2, unsigned int, 4)
MEMSET1A(cuMemsetD8Async, unsigned char, 1)
MEMSET1A(cuMemsetD16Async, unsigned short, 2)
MEMSET1A(cuMemsetD32Async, unsigned int, 4)
MEMSET2(cuMemsetD2D8_v2, unsigned char, 1)
MEMSET2(cuMemsetD2D16_v2, unsigned short, 2)
MEMSET2(cuMemsetD2D32_v2, unsigned int, 4)
MEMSET2A(cuMemsetD2D8Async, unsigned char, 1)
MEMSET2A(cuMemsetD2D16Async, unsigned short, 2)
MEMSET2A(cuMemsetD2D32Async, unsigned int, 4)

// --- modules and functions

C2CAPI CUresult cuModuleLoadData(CUmodule* m, const void* image) {
    CUresult r = REAL(cuModuleLoadData)(m, image);
    if (!r && on_remote()) register_module(*m, image, false);
    return r;
}
C2CAPI CUresult cuModuleLoadDataEx(CUmodule* m, const void* image, unsigned n, CUjit_option* o, void** v) {
    CUresult r = REAL(cuModuleLoadDataEx)(m, image, n, o, v);
    if (!r && on_remote()) register_module(*m, image, false);
    return r;
}
C2CAPI CUresult cuModuleLoadFatBinary(CUmodule* m, const void* fatbin) {
    CUresult r = REAL(cuModuleLoadFatBinary)(m, fatbin);
    if (!r && on_remote()) register_module(*m, fatbin, false);
    return r;
}
C2CAPI CUresult cuModuleLoad(CUmodule* m, const char* fname) {
    CUresult r = REAL(cuModuleLoad)(m, fname);
    if (!r && on_remote()) {
        FILE* f = std::fopen(fname, "rb");
        if (!f) return r;
        std::vector<uint8_t> d;
        uint8_t b[65536];
        size_t k;
        while ((k = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + k);
        std::fclose(f);
        d.push_back(0);  // PTX has to be NUL terminated
        register_module(*m, d.data(), false);
    }
    return r;
}
C2CAPI CUresult cuModuleUnload(CUmodule m) {
    if (S().fd >= 0) forget_module(m);
    return REAL(cuModuleUnload)(m);
}

C2CAPI CUresult cuModuleGetFunction(CUfunction* f, CUmodule m, const char* name) {
    CUresult r = REAL(cuModuleGetFunction)(f, m, name);
    State& s = S();
    if (r || s.fd < 0) return r;
    std::lock_guard<std::recursive_mutex> g(s.mu);
    auto it = s.mods.find(m);
    if (it == s.mods.end()) return r;
    auto old = s.funcs.find(*f);
    if (old != s.funcs.end() && old->second->mod == it->second && old->second->name == name) return r;
    if (old != s.funcs.end()) delete old->second;
    s.funcs[*f] = new Func{it->second, name, s.next_id++};
    return r;
}

C2CAPI CUresult cuModuleGetGlobal_v2(CUdeviceptr* dp, size_t* bytes, CUmodule m, const char* name) {
    State& s = S();
    if (s.fd >= 0) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        auto it = s.mods.find(m);
        if (it != s.mods.end()) {
            Mod* md = it->second;
            auto key = std::make_pair(md, std::string(name));
            auto a = s.alias_of.find(key);
            if (a == s.alias_of.end()) {
                if (!ensure_module_sent(md)) return CUDA_ERROR_NO_BINARY_FOR_GPU;
                MsgGlobal q{md->mid, uint32_t(std::strlen(name))};
                MsgGlobalReply rep{0, 0};
                CUresult r = call(OP_GLOBAL_GET, &q, sizeof q, &rep, sizeof rep, name, q.namelen);
                if (r) return r;
                if (!s.alias_base) {  // separate VA range just for symbol aliases
                    CUdeviceptr w = 0;
                    if (REAL(cuMemAddressReserve)(&w, 256u << 20, kGran, 0, 0)) return CUDA_ERROR_OUT_OF_MEMORY;
                    s.alias_base = s.alias_next = w;
                    s.alias_end = w + (256u << 20);
                }
                uint64_t al = round_up(s.alias_next, 256);
                if (al + std::max<uint64_t>(rep.size, 1) > s.alias_end) return CUDA_ERROR_OUT_OF_MEMORY;
                s.alias_next = al + std::max<uint64_t>(rep.size, 1);
                s.ranges[al] = {rep.size, 0, Kind::Alias, rep.ptr};
                a = s.alias_of.emplace(key, al).first;
            }
            uint64_t b;
            const Range* rg = find_range(a->second, &b);
            if (dp) *dp = a->second;
            if (bytes) *bytes = rg ? rg->size : 0;
            return CUDA_SUCCESS;
        }
    }
    return REAL(cuModuleGetGlobal_v2)(dp, bytes, m, name);
}

C2CAPI CUresult cuFuncSetAttribute(CUfunction f, CUfunction_attribute a, int v) {
    CUresult r = REAL(cuFuncSetAttribute)(f, a, v);
    if (Func* rf = remote_func(f)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        if (rf->sent) {
            MsgFuncAttr m{rf->fid, a, v};
            post(OP_FUNC_ATTR, &m, sizeof m);
        } else {
            rf->attrs.push_back({a, v});
        }
    }
    return r;
}

C2CAPI CUresult cuFuncSetCacheConfig(CUfunction f, CUfunc_cache c) {
    CUresult r = REAL(cuFuncSetCacheConfig)(f, c);
    if (Func* rf = remote_func(f)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        rf->cache = c;
        if (rf->sent) {
            MsgFuncAttr m{rf->fid, 0, c};
            post(OP_FUNC_CACHE, &m, sizeof m);
        }
    }
    return r;
}

C2CAPI CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx,
                               unsigned by, unsigned bz, unsigned shmem, CUstream st, void** params,
                               void** extra) {
    if (Func* rf = remote_func(f)) return r_launch(rf, gx, gy, gz, bx, by, bz, shmem, st, params, extra);
    return REAL(cuLaunchKernel)(f, gx, gy, gz, bx, by, bz, shmem, st, params, extra);
}

C2CAPI CUresult cuLaunchCooperativeKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx,
                                          unsigned by, unsigned bz, unsigned shmem, CUstream st, void** params) {
    if (Func* rf = remote_func(f)) return r_launch(rf, gx, gy, gz, bx, by, bz, shmem, st, params, nullptr, true);
    return REAL(cuLaunchCooperativeKernel)(f, gx, gy, gz, bx, by, bz, shmem, st, params);
}

// --- streams and events

static CUresult stream_create(CUstream* st, unsigned flags, int prio, bool with_prio) {
    CUresult r = with_prio ? REAL(cuStreamCreateWithPriority)(st, flags, prio) : REAL(cuStreamCreate)(st, flags);
    if (r || !on_remote()) return r;
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    s.streams.insert(*st);
    MsgStream m{reinterpret_cast<uint64_t>(*st), flags, prio};
    post(OP_STREAM_CREATE, &m, sizeof m);
    return r;
}
C2CAPI CUresult cuStreamCreate(CUstream* st, unsigned flags) { return stream_create(st, flags, 0, false); }
C2CAPI CUresult cuStreamCreateWithPriority(CUstream* st, unsigned flags, int prio) {
    return stream_create(st, flags, prio, true);
}

C2CAPI CUresult cuStreamDestroy_v2(CUstream st) {
    State& s = S();
    if (s.fd >= 0) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        if (s.streams.erase(st)) {
            uint64_t sid = reinterpret_cast<uint64_t>(st);
            post(OP_STREAM_DESTROY, &sid, 8);
        }
    }
    return REAL(cuStreamDestroy_v2)(st);
}

C2CAPI CUresult cuStreamSynchronize(CUstream st) {
    if (is_remote_stream(st)) {
        State& s = S();
        std::lock_guard<std::recursive_mutex> g(s.mu);
        uint64_t sid = sid_of(st);
        if (s.post_seq == s.clean_seq && (s.clean_all || s.clean_sid == sid))
            return CUresult(s.deferred_err.exchange(0));  // nothing new since last drain
        CUresult r = call(OP_STREAM_SYNC, &sid, 8);
        if (r == CUDA_SUCCESS) {
            s.clean_seq = s.post_seq;
            s.clean_sid = sid;
            s.clean_all = false;
        }
        return r;
    }
    return REAL(cuStreamSynchronize)(st);
}

C2CAPI CUresult cuStreamQuery(CUstream st) {
    if (is_remote_stream(st)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        uint64_t sid = sid_of(st);
        return call(OP_STREAM_QUERY, &sid, 8);
    }
    return REAL(cuStreamQuery)(st);
}

static bool is_remote_event(CUevent e) {
    State& s = S();
    if (s.fd < 0) return false;
    std::lock_guard<std::recursive_mutex> g(s.mu);
    return s.events.count(e) != 0;
}

C2CAPI CUresult cuStreamWaitEvent(CUstream st, CUevent e, unsigned flags) {
    bool rs = is_remote_stream(st), re = is_remote_event(e);
    std::lock_guard<std::recursive_mutex> g(S().mu);
    if (rs && re) {
        MsgWait m{sid_of(st), reinterpret_cast<uint64_t>(e), flags};
        return post(OP_STREAM_WAIT, &m, sizeof m) ? CUDA_SUCCESS : kLinkErr;
    }
    if (rs) return REAL(cuEventSynchronize)(e);  // remote stream, local event
    if (re) {                                    // local stream, remote event
        uint64_t eid = reinterpret_cast<uint64_t>(e);
        return call(OP_EVENT_SYNC, &eid, 8);
    }
    return REAL(cuStreamWaitEvent)(st, e, flags);
}

C2CAPI CUresult cuStreamAddCallback(CUstream st, CUstreamCallback cb, void* ud, unsigned flags) {
    if (is_remote_stream(st)) {
        CUresult r = cuStreamSynchronize(st);
        cb(st, r, ud);
        return CUDA_SUCCESS;
    }
    return REAL(cuStreamAddCallback)(st, cb, ud, flags);
}

C2CAPI CUresult cuLaunchHostFunc(CUstream st, CUhostFn fn, void* ud) {
    if (is_remote_stream(st)) {
        CUresult r = cuStreamSynchronize(st);
        if (r) return r;
        fn(ud);
        return CUDA_SUCCESS;
    }
    return REAL(cuLaunchHostFunc)(st, fn, ud);
}

C2CAPI CUresult cuEventCreate(CUevent* e, unsigned flags) {
    CUresult r = REAL(cuEventCreate)(e, flags);
    if (r || !on_remote()) return r;
    State& s = S();
    std::lock_guard<std::recursive_mutex> g(s.mu);
    s.events.insert(*e);
    MsgEvent m{reinterpret_cast<uint64_t>(*e), flags & ~unsigned(CU_EVENT_INTERPROCESS)};
    post(OP_EVENT_CREATE, &m, sizeof m);
    return r;
}

C2CAPI CUresult cuEventDestroy_v2(CUevent e) {
    State& s = S();
    if (s.fd >= 0) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        if (s.events.erase(e)) {
            uint64_t eid = reinterpret_cast<uint64_t>(e);
            post(OP_EVENT_DESTROY, &eid, 8);
        }
    }
    return REAL(cuEventDestroy_v2)(e);
}

static CUresult event_record(CUevent e, CUstream st) {
    if (!is_remote_event(e)) return CUDA_ERROR_INVALID_HANDLE;
    if (!is_remote_stream(st)) return CUDA_ERROR_INVALID_HANDLE;  // event + stream on different GPUs
    std::lock_guard<std::recursive_mutex> g(S().mu);
    MsgRecord m{reinterpret_cast<uint64_t>(e), sid_of(st)};
    return post(OP_EVENT_RECORD, &m, sizeof m) ? CUDA_SUCCESS : kLinkErr;
}
C2CAPI CUresult cuEventRecord(CUevent e, CUstream st) {
    if (is_remote_event(e)) return event_record(e, st);
    return REAL(cuEventRecord)(e, st);
}
C2CAPI CUresult cuEventRecordWithFlags(CUevent e, CUstream st, unsigned flags) {
    if (is_remote_event(e)) return event_record(e, st);
    return REAL(cuEventRecordWithFlags)(e, st, flags);
}

C2CAPI CUresult cuEventSynchronize(CUevent e) {
    if (is_remote_event(e)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        uint64_t eid = reinterpret_cast<uint64_t>(e);
        return call(OP_EVENT_SYNC, &eid, 8);
    }
    return REAL(cuEventSynchronize)(e);
}
C2CAPI CUresult cuEventQuery(CUevent e) {
    if (is_remote_event(e)) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        uint64_t eid = reinterpret_cast<uint64_t>(e);
        return call(OP_EVENT_QUERY, &eid, 8);
    }
    return REAL(cuEventQuery)(e);
}
C2CAPI CUresult cuEventElapsedTime(float* ms, CUevent a, CUevent b) {
    bool ra = is_remote_event(a), rb = is_remote_event(b);
    if (ra != rb) return CUDA_ERROR_INVALID_HANDLE;
    if (ra) {
        std::lock_guard<std::recursive_mutex> g(S().mu);
        MsgElapsed m{reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b)};
        return call(OP_EVENT_ELAPSED, &m, sizeof m, ms, sizeof *ms);
    }
    return REAL(cuEventElapsedTime)(ms, a, b);
}

// --- _ptsz/_ptds variants (nvcc --default-stream per-thread)

extern "C" {
CUresult cuLaunchKernel_ptsz(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                             unsigned, CUstream, void**, void**);
CUresult cuMemcpy_ptds(CUdeviceptr, CUdeviceptr, size_t);
CUresult cuMemcpyAsync_ptsz(CUdeviceptr, CUdeviceptr, size_t, CUstream);
CUresult cuMemcpyHtoD_v2_ptds(CUdeviceptr, const void*, size_t);
CUresult cuMemcpyDtoH_v2_ptds(void*, CUdeviceptr, size_t);
CUresult cuMemcpyDtoD_v2_ptds(CUdeviceptr, CUdeviceptr, size_t);
CUresult cuMemcpyHtoDAsync_v2_ptsz(CUdeviceptr, const void*, size_t, CUstream);
CUresult cuMemcpyDtoHAsync_v2_ptsz(void*, CUdeviceptr, size_t, CUstream);
CUresult cuMemcpyDtoDAsync_v2_ptsz(CUdeviceptr, CUdeviceptr, size_t, CUstream);
CUresult cuMemsetD8_v2_ptds(CUdeviceptr, unsigned char, size_t);
CUresult cuMemsetD32_v2_ptds(CUdeviceptr, unsigned int, size_t);
CUresult cuMemsetD8Async_ptsz(CUdeviceptr, unsigned char, size_t, CUstream);
CUresult cuMemsetD32Async_ptsz(CUdeviceptr, unsigned int, size_t, CUstream);
CUresult cuStreamSynchronize_ptsz(CUstream);
CUresult cuStreamQuery_ptsz(CUstream);
CUresult cuStreamWaitEvent_ptsz(CUstream, CUevent, unsigned);
CUresult cuEventRecord_ptsz(CUevent, CUstream);
CUresult cuLaunchHostFunc_ptsz(CUstream, CUhostFn, void*);
CUresult cuLaunchCooperativeKernel_ptsz(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                        unsigned, CUstream, void**);
CUresult cuStreamAddCallback_ptsz(CUstream, CUstreamCallback, void*, unsigned);
}
static CUstream pt(CUstream st) { return st ? st : CU_STREAM_PER_THREAD; }

C2CAPI CUresult cuLaunchKernel_ptsz(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx,
                                    unsigned by, unsigned bz, unsigned shmem, CUstream st, void** params,
                                    void** extra) {
    if (Func* rf = remote_func(f)) return r_launch(rf, gx, gy, gz, bx, by, bz, shmem, pt(st), params, extra);
    return REAL(cuLaunchKernel_ptsz)(f, gx, gy, gz, bx, by, bz, shmem, st, params, extra);
}
C2CAPI CUresult cuLaunchCooperativeKernel_ptsz(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx,
                                               unsigned by, unsigned bz, unsigned shmem, CUstream st,
                                               void** params) {
    if (Func* rf = remote_func(f))
        return r_launch(rf, gx, gy, gz, bx, by, bz, shmem, pt(st), params, nullptr, true);
    return REAL(cuLaunchCooperativeKernel_ptsz)(f, gx, gy, gz, bx, by, bz, shmem, st, params);
}
C2CAPI CUresult cuMemcpy_ptds(CUdeviceptr d, CUdeviceptr s, size_t n) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, CU_STREAM_PER_THREAD, false);
    return REAL(cuMemcpy_ptds)(d, s, n);
}
C2CAPI CUresult cuMemcpyAsync_ptsz(CUdeviceptr d, CUdeviceptr s, size_t n, CUstream st) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, pt(st), true);
    return REAL(cuMemcpyAsync_ptsz)(d, s, n, st);
}
C2CAPI CUresult cuMemcpyHtoD_v2_ptds(CUdeviceptr d, const void* h, size_t n) {
    if (is_remote_ptr(d)) return r_htod(d, h, n, kStreamPerThread);
    return REAL(cuMemcpyHtoD_v2_ptds)(d, h, n);
}
C2CAPI CUresult cuMemcpyDtoH_v2_ptds(void* h, CUdeviceptr d, size_t n) {
    if (is_remote_ptr(d)) return r_dtoh(h, d, n, kStreamPerThread);
    return REAL(cuMemcpyDtoH_v2_ptds)(h, d, n);
}
C2CAPI CUresult cuMemcpyDtoD_v2_ptds(CUdeviceptr d, CUdeviceptr s, size_t n) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, CU_STREAM_PER_THREAD, false);
    return REAL(cuMemcpyDtoD_v2_ptds)(d, s, n);
}
C2CAPI CUresult cuMemcpyHtoDAsync_v2_ptsz(CUdeviceptr d, const void* h, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_htod(d, h, n, STREAM_ARG(pt(st)));
    return REAL(cuMemcpyHtoDAsync_v2_ptsz)(d, h, n, st);
}
C2CAPI CUresult cuMemcpyDtoHAsync_v2_ptsz(void* h, CUdeviceptr d, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_dtoh_async(h, d, n, STREAM_ARG(pt(st)));
    return REAL(cuMemcpyDtoHAsync_v2_ptsz)(h, d, n, st);
}
C2CAPI CUresult cuMemcpyDtoDAsync_v2_ptsz(CUdeviceptr d, CUdeviceptr s, size_t n, CUstream st) {
    if (is_remote_ptr(d) || is_remote_ptr(s)) return copy_any(d, s, n, pt(st), true);
    return REAL(cuMemcpyDtoDAsync_v2_ptsz)(d, s, n, st);
}
C2CAPI CUresult cuMemsetD8_v2_ptds(CUdeviceptr d, unsigned char v, size_t n) {
    if (is_remote_ptr(d)) return r_memset(d, 0, v, 1, n, 1, kStreamPerThread);
    return REAL(cuMemsetD8_v2_ptds)(d, v, n);
}
C2CAPI CUresult cuMemsetD32_v2_ptds(CUdeviceptr d, unsigned v, size_t n) {
    if (is_remote_ptr(d)) return r_memset(d, 0, v, 4, n, 1, kStreamPerThread);
    return REAL(cuMemsetD32_v2_ptds)(d, v, n);
}
C2CAPI CUresult cuMemsetD8Async_ptsz(CUdeviceptr d, unsigned char v, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_memset(d, 0, v, 1, n, 1, STREAM_ARG(pt(st)));
    return REAL(cuMemsetD8Async_ptsz)(d, v, n, st);
}
C2CAPI CUresult cuMemsetD32Async_ptsz(CUdeviceptr d, unsigned v, size_t n, CUstream st) {
    if (is_remote_ptr(d)) return r_memset(d, 0, v, 4, n, 1, STREAM_ARG(pt(st)));
    return REAL(cuMemsetD32Async_ptsz)(d, v, n, st);
}
C2CAPI CUresult cuStreamSynchronize_ptsz(CUstream st) {
    if (is_remote_stream(pt(st))) return cuStreamSynchronize(pt(st));
    return REAL(cuStreamSynchronize_ptsz)(st);
}
C2CAPI CUresult cuStreamQuery_ptsz(CUstream st) {
    if (is_remote_stream(pt(st))) return cuStreamQuery(pt(st));
    return REAL(cuStreamQuery_ptsz)(st);
}
C2CAPI CUresult cuStreamWaitEvent_ptsz(CUstream st, CUevent e, unsigned flags) {
    if (is_remote_stream(pt(st)) || is_remote_event(e)) return cuStreamWaitEvent(pt(st), e, flags);
    return REAL(cuStreamWaitEvent_ptsz)(st, e, flags);
}
C2CAPI CUresult cuEventRecord_ptsz(CUevent e, CUstream st) {
    if (is_remote_event(e)) return event_record(e, pt(st));
    return REAL(cuEventRecord_ptsz)(e, st);
}
C2CAPI CUresult cuLaunchHostFunc_ptsz(CUstream st, CUhostFn fn, void* ud) {
    if (is_remote_stream(pt(st))) return cuLaunchHostFunc(pt(st), fn, ud);
    return REAL(cuLaunchHostFunc_ptsz)(st, fn, ud);
}
C2CAPI CUresult cuStreamAddCallback_ptsz(CUstream st, CUstreamCallback cb, void* ud, unsigned flags) {
    if (is_remote_stream(pt(st))) return cuStreamAddCallback(pt(st), cb, ud, flags);
    return REAL(cuStreamAddCallback_ptsz)(st, cb, ud, flags);
}

// --- entry-point lookup and NVIDIA's private tables

// cudart grabs almost every driver function through cuGetProcAddress, so we
// hand back ours (or a trampoline) for each one. Including cuGetProcAddress
// and cuGetExportTable themselves, otherwise cudart ends up on the real ones.
C2CAPI CUresult cuGetExportTable(const void** tbl, const CUuuid* id);
C2CAPI CUresult cuGetProcAddress(const char* sym, void** pfn, int ver, cuuint64_t flags) {
    load_real();
    CUresult r = REAL(cuGetProcAddress)(sym, pfn, ver, flags);
    if (r || !pfn || !*pfn) return r;
    if (!std::strcmp(sym, "cuGetProcAddress")) {
        *pfn = reinterpret_cast<void*>(&cuGetProcAddress);
        return r;
    }
    if (!std::strcmp(sym, "cuGetExportTable")) {
        *pfn = reinterpret_cast<void*>(&cuGetExportTable);
        return r;
    }
    auto it = g_real_index.find(*pfn);
    if (it != g_real_index.end()) *pfn = c2c_tramp_table[it->second];
    else dbg("cuGetProcAddress(%s): unknown entry point, passed through", sym);
    return r;
}

// Hidden table 6bd5fb6c-...: slot 6 is what cudart uses to load the fatbins an
// app (or cuBLAS etc.) registered into a ctx. We hook it so we know about
// modules that get loaded into a remote ctx.
namespace {
const uint8_t kRuntimeTable[16] = {0x6b, 0xd5, 0xfb, 0x6c, 0x5b, 0xf4, 0xe7, 0x4a,
                                   0x89, 0x87, 0xd9, 0x39, 0x12, 0xfd, 0x9d, 0xf9};
using Slot6 = CUresult (*)(CUmodule*, const void*, void*, void*, void*, void*);
Slot6 g_slot6 = nullptr;
// slot 2: (out, dev ordinal, ...) fills `out` with the device's primary ctx
// (first field) plus some state. cudart uses it instead of
// cuDevicePrimaryCtxRetain. Has to return the shadow ctx for the remote dev,
// else you get error 709 (context destroyed).
using Slot2 = CUresult (*)(void*, long, void*, void*, void*, void*);
Slot2 g_slot2 = nullptr;
void* g_rt_table[64];

CUresult slot2(void* out, long dev, void* a, void* b, void* c, void* d) {
    State& s = S();
    if (remote_dev(CUdevice(dev))) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        CUresult r = g_slot2(out, 0, a, b, c, d);  // fill it like for the local GPU first
        if (r) return r;
        if (!s.prim) {
            r = make_shadow(&s.prim, s.prim_flags);
            if (r) return r;
            REAL(cuCtxPopCurrent_v2)(nullptr);
        }
        *static_cast<CUcontext*>(out) = s.prim;
        dbg("runtime primary context for remote device %ld -> shadow %p", dev, (void*)s.prim);
        return CUDA_SUCCESS;
    }
    int rd = real_dev(CUdevice(dev));
    return g_slot2(out, rd < 0 ? dev : rd, a, b, c, d);
}

CUresult slot6(CUmodule* m, const void* wrapper, void* a, void* b, void* c, void* d) {
    CUresult r = g_slot6(m, wrapper, a, b, c, d);
    if (!r && m && *m && on_remote()) register_module(*m, wrapper, true);
    return r;
}
}  // namespace

C2CAPI CUresult cuGetExportTable(const void** tbl, const CUuuid* id) {
    load_real();
    CUresult r = REAL(cuGetExportTable)(tbl, id);
    if (r || !tbl || !*tbl || !id || std::memcmp(id->bytes, kRuntimeTable, 16) != 0) return r;
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [&] {
        auto* t = static_cast<void* const*>(*tbl);
        size_t bytes = reinterpret_cast<size_t>(t[0]);
        size_t n = bytes / sizeof(void*);
        if (bytes % sizeof(void*) || n < 7 || n > 64) {
            warnf("unexpected CUDA runtime table layout; remote GPU modules disabled");
            return;
        }
        std::memcpy(g_rt_table, t, bytes);
        g_slot6 = reinterpret_cast<Slot6>(t[6]);
        g_rt_table[6] = reinterpret_cast<void*>(&slot6);
        g_slot2 = reinterpret_cast<Slot2>(t[2]);
        g_rt_table[2] = reinterpret_cast<void*>(&slot2);
        ok = true;
    });
    if (ok) *tbl = g_rt_table;
    return r;
}

// C2C_GPU_STATS=1 prints these at exit (tests grep the "via" part)
__attribute__((destructor)) static void c2c_fini() {
    State& s = S();
    if (s.fd >= 0) {
        std::lock_guard<std::recursive_mutex> g(s.mu);
        flush();
    }
    if (s.fd >= 0 && getenv("C2C_GPU_STATS"))
        std::fprintf(stderr,
                     "[c2c-gpu] remote GPU via %s: %llu kernel launches, %llu round trips, %llu async "
                     "downloads, %.1f MiB sent, %.1f MiB received\n",
                     s.via.c_str(), (unsigned long long)s.n_launch, (unsigned long long)s.n_rpc,
                     (unsigned long long)s.n_async_dtoh, s.b_up / 1048576.0, s.b_down / 1048576.0);
}
