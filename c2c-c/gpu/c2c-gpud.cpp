// c2c-gpud: lends this board's GPU to the C2C master.
//
// Runs on the slave. Each app on the master that touches the remote GPU
// (via libc2ccuda) opens one connection, and I fork a child per connection
// that owns its own CUDA context - same as a normal local CUDA process. So if
// one app crashes or leaks, nobody else cares, and the driver cleans it all
// up when the app goes away.
//
// Allocations get mapped at the VA the master reserved for them (CUDA VMM),
// so device pointers are the same on both sides and can go straight into
// kernel args.
//
//   c2c-gpud [-l 10.77.0.2] [-p 10810] [-a 10.77.0.1] [-c /var/cache/c2c-gpud]
#include <arpa/inet.h>
#include <cuda.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu_proto.hpp"

using namespace c2cgpu;

static void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "c2c-gpud[%d]: ", getpid());
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

// what this session talks over (set in the session process): a raw USB
// channel (/dev/c2cchN, non-blocking, busy-polled) or plain TCP
static bool g_raw = false;
static int g_spin_us = 1000;

// GPU description we send in every hello. A throwaway helper process grabs it
// once at startup. The main server must never init CUDA itself (it forks every
// session, and CUDA + fork don't mix), and a session that never does GPU work
// never makes a context either - see Session::ensure
static ServerHello g_desc{};
// pinned download buffer per session (--download-buffer-mib). This is how much
// download can be in flight while uploads keep coming in
static int g_pool_max = 64;  // x 8 MiB
static bool g_have_desc = false;

static double now_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

// single read: >0 bytes, 0 closed/reset, <0 error. raw channel spins a bit, then sleeps
static ssize_t read_some(int fd, void* p, size_t n, bool waitall) {
    double spin_from = 0;
    for (;;) {
        ssize_t r = g_raw ? read(fd, p, n) : recv(fd, p, n, waitall ? MSG_WAITALL : 0);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN && g_raw) {
            if (g_spin_us > 0) {
                double t = now_us();
                if (spin_from == 0) spin_from = t;
                if (t - spin_from < g_spin_us) continue;
            }
            pollfd pf{fd, POLLIN, 0};
            poll(&pf, 1, 100);
            continue;
        }
        return -1;  // ECONNRESET lands here too = master closed the channel
    }
}

static bool recv_all(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = read_some(fd, p, n, true);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

static bool send_all(int fd, const void* buf, size_t n) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (n) {
        ssize_t r = g_raw ? write(fd, p, n) : send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && errno == EAGAIN) {
                pollfd pf{fd, POLLOUT, 0};
                poll(&pf, 1, 100);
                continue;
            }
            return false;
        }
        p += r;
        n -= size_t(r);
    }
    return true;
}

static std::string hash_name(const uint64_t h[2]) {
    char b[40];
    std::snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)h[0], (unsigned long long)h[1]);
    return b;
}

namespace {

// buffered reader: one recv() pulls up to 1 MiB and we parse messages out of
// that. A launch is ~70 bytes, one syscall per launch would kill the launch rate
struct Reader {
    int fd = -1;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1u << 20);
    size_t head = 0, tail = 0;
    bool fill() {
        if (head == tail) head = tail = 0;
        if (tail == buf.size()) {  // compact
            std::memmove(buf.data(), buf.data() + head, tail - head);
            tail -= head;
            head = 0;
        }
        ssize_t r = read_some(fd, buf.data() + tail, buf.size() - tail, false);
        if (r <= 0) return false;
        tail += size_t(r);
        return true;
    }
    bool read(void* dst, size_t n) {
        auto* d = static_cast<uint8_t*>(dst);
        while (n) {
            if (head == tail) {
                // big reads skip the buffer
                if (n >= buf.size() / 2) return recv_all(fd, d, n);
                if (!fill()) return false;
            }
            size_t c = std::min(n, tail - head);
            std::memcpy(d, buf.data() + head, c);
            head += c;
            d += c;
            n -= c;
        }
        return true;
    }
};

struct Session {
    int fd;
    Reader rd;
    bool profile = false;
    struct OpStat { uint64_t n = 0; double sec = 0; };
    std::map<uint32_t, OpStat> prof;
    std::string cache_dir;
    CUdevice dev = 0;
    CUcontext ctx = nullptr;
    CUresult async_err = CUDA_SUCCESS;
    std::unordered_map<uint64_t, CUstream> streams;
    std::unordered_map<uint64_t, CUevent> events;
    std::unordered_map<uint32_t, CUmodule> modules;
    std::unordered_map<uint32_t, CUfunction> funcs;
    std::unordered_map<uint64_t, CUarray> arrays;
    std::set<CUtexObject> texs;
    std::set<CUsurfObject> surfs;
    struct Alloc { size_t size; CUmemGenericAllocationHandle h; };
    std::map<uint64_t, Alloc> allocs;
    uint64_t win_base = 0, win_size = 0;  // shared device-address window
    void* stage[2] = {nullptr, nullptr};
    CUevent stage_ev[2] = {nullptr, nullptr};
    std::vector<uint8_t> buf;
    MsgStats st{};

    // Everything going back to the master goes through one ordered queue that
    // the tx thread drains. Downloads land in a pool of pinned buffers and get
    // queued; tx sends each one once its copy is done. Meanwhile the request
    // side keeps reading uploads/launches, so the link is busy both ways.
    struct TxJob {
        std::vector<uint8_t> bytes;  // header (+ small payload)
        int slot = -1;               // pool buffer to send after bytes
        size_t len = 0;
    };
    // grows on demand up to the limit. If a download is bigger than the pool
    // the request side stalls, and the uploads stall with it
    static constexpr int kPoolInit = 8, kPoolMax = 512;  // hard cap 4 GiB
    void* pool[kPoolMax] = {};
    CUevent pool_ev[kPoolMax] = {};
    int pool_n = 0;
    std::vector<int> pool_free;
    std::mutex txmu;
    std::condition_variable txcv;
    std::deque<TxJob> txq;
    bool tx_busy = false;  // tx thread or an inline sender is mid-write
    bool tx_dead = false;
    std::atomic<int> tx_async_err{0};

    void tx_loop() {
        cuCtxSetCurrent(ctx);
        for (;;) {
            TxJob j;
            {
                std::unique_lock<std::mutex> lk(txmu);
                txcv.wait(lk, [&] { return !txq.empty() && !tx_busy; });
                j = std::move(txq.front());
                txq.pop_front();
                tx_busy = true;
            }
            bool ok = j.bytes.empty() || send_all(fd, j.bytes.data(), j.bytes.size());
            if (j.slot >= 0) {
                CUresult r = cuEventSynchronize(pool_ev[j.slot]);
                if (r) {
                    int zero = 0;
                    tx_async_err.compare_exchange_strong(zero, int(r));
                }
                ok = ok && send_all(fd, pool[j.slot], j.len);
                std::lock_guard<std::mutex> g(txmu);
                pool_free.push_back(j.slot);
            }
            std::lock_guard<std::mutex> g(txmu);
            tx_busy = false;
            if (!ok) tx_dead = true;
            txcv.notify_all();
            if (!ok) return;
        }
    }
    // Small reply and nothing queued ahead of it? Write it right here, no
    // thread wakeup on the latency path. Anything else waits its turn.
    bool enqueue(TxJob&& j) {
        std::unique_lock<std::mutex> lk(txmu);
        if (tx_dead) return false;
        if (j.slot < 0 && txq.empty() && !tx_busy) {
            tx_busy = true;
            lk.unlock();
            bool ok = send_all(fd, j.bytes.data(), j.bytes.size());
            lk.lock();
            tx_busy = false;
            if (!ok) tx_dead = true;
            txcv.notify_all();
            return ok;
        }
        txq.push_back(std::move(j));
        txcv.notify_all();
        return true;
    }
    bool grow_pool() {  // request thread only, it owns pool_n
        if (pool_n >= std::min(kPoolMax, g_pool_max)) return false;
        int i = pool_n;
        if (cuMemHostAlloc(&pool[i], kChunk, 0) != CUDA_SUCCESS) return false;
        if (cuEventCreate(&pool_ev[i], CU_EVENT_DISABLE_TIMING) != CUDA_SUCCESS) {
            cuMemFreeHost(pool[i]);
            return false;
        }
        pool_n++;
        std::lock_guard<std::mutex> g(txmu);
        pool_free.push_back(i);
        return true;
    }
    int take_slot() {  // get a free download buffer: reuse, grow or wait
        {
            std::lock_guard<std::mutex> g(txmu);
            if (pool_free.empty() && pool_n < std::min(kPoolMax, g_pool_max)) {
                // grow below, outside the lock
            } else if (!pool_free.empty()) {
                int i = pool_free.back();
                pool_free.pop_back();
                return i;
            }
        }
        grow_pool();
        std::unique_lock<std::mutex> lk(txmu);
        txcv.wait(lk, [&] { return !pool_free.empty() || tx_dead; });
        if (tx_dead) return -1;
        int i = pool_free.back();
        pool_free.pop_back();
        return i;
    }

    void note(CUresult r) {
        if (r != CUDA_SUCCESS && async_err == CUDA_SUCCESS) async_err = r;
    }
    // sync op result: its own error wins, otherwise hand back a pending async one
    CUresult sync_result(CUresult r) {
        if (r != CUDA_SUCCESS) return r;
        if (int e = tx_async_err.exchange(0)) note(CUresult(e));
        r = async_err;
        async_err = CUDA_SUCCESS;
        return r;
    }
    bool reply(CUresult r, const void* data = nullptr, uint32_t n = 0) {
        TxJob j;
        ReplyHdr h{int32_t(r), n};
        j.bytes.resize(sizeof h + n);
        std::memcpy(j.bytes.data(), &h, sizeof h);
        if (n) std::memcpy(j.bytes.data() + sizeof h, data, n);
        return enqueue(std::move(j));
    }
    CUstream stream(uint64_t sid, bool* ok = nullptr) {
        if (ok) *ok = true;
        if (sid == kStreamLegacy) return nullptr;
        if (sid == kStreamPerThread) return CU_STREAM_PER_THREAD;
        auto it = streams.find(sid);
        if (it != streams.end()) return it->second;
        if (ok) *ok = false;
        return nullptr;
    }
    bool payload(uint32_t len) {
        buf.resize(len);
        return !len || rd.read(buf.data(), len);
    }

    bool init() {
        CUresult r = cuInit(0);
        if (!r) r = cuDeviceGet(&dev, 0);
        if (!r) r = cuDevicePrimaryCtxRetain(&ctx, dev);
        if (!r) r = cuCtxSetCurrent(ctx);
        for (int i = 0; !r && i < 2; i++) {
            r = cuMemHostAlloc(&stage[i], kChunk, 0);
            if (!r) r = cuEventCreate(&stage_ev[i], CU_EVENT_DISABLE_TIMING);
        }
        for (int i = 0; !r && i < kPoolInit; i++)
            if (!grow_pool()) r = CUDA_ERROR_OUT_OF_MEMORY;
        if (r) {
            const char* s = "?";
            cuGetErrorString(r, &s);
            logf("CUDA init failed: %s", s);
        }
        return r == CUDA_SUCCESS;
    }

    // first real GPU request: make the CUDA context + tx thread now, so apps
    // that never use the remote GPU cost us nothing
    bool started = false;
    bool ensure() {
        if (started) return true;
        started = true;
        if (!init()) return false;
        std::thread(&Session::tx_loop, this).detach();
        return true;
    }

    bool hello() {
        char magic[8];
        uint32_t ver = 0;
        rd.fd = fd;
        if (!rd.read(magic, 8) || !rd.read(&ver, 4)) return false;
        if (std::memcmp(magic, kHelloClient, 8) != 0 || ver != kProtoVersion) {
            logf("bad hello / protocol version %u", ver);
            return false;
        }
        ServerHello h = g_desc;
        std::vector<uint8_t> hashes;
        if (DIR* d = opendir(cache_dir.c_str())) {
            while (dirent* e = readdir(d)) {
                if (std::strlen(e->d_name) != 32) continue;
                uint64_t hh[2];
                char a[17] = {0}, b[17] = {0};
                std::memcpy(a, e->d_name, 16);
                std::memcpy(b, e->d_name + 16, 16);
                hh[0] = std::strtoull(a, nullptr, 16);
                hh[1] = std::strtoull(b, nullptr, 16);
                hashes.insert(hashes.end(), reinterpret_cast<uint8_t*>(hh),
                              reinterpret_cast<uint8_t*>(hh) + 16);
            }
            closedir(d);
        }
        h.ncached = uint32_t(hashes.size() / 16);
        return send_all(fd, &h, sizeof h) && (hashes.empty() || send_all(fd, hashes.data(), hashes.size()));
    }

    // Device addresses have to match on both ends. Client and server both
    // reserve the same VA window; the client picks addresses inside it and we
    // put real memory behind them.
    CUresult va_probe(uint64_t size, uint64_t* where) {
        CUdeviceptr p = 0;
        CUresult r = cuMemAddressReserve(&p, size, 0, 0, 0);
        if (r) return r;
        cuMemAddressFree(p, size);
        *where = p;
        return CUDA_SUCCESS;
    }
    CUresult va_window(uint64_t base, uint64_t size) {
        if (win_size) return base == win_base && size == win_size ? CUDA_SUCCESS : CUDA_ERROR_INVALID_VALUE;
        uint64_t start = 0;
        CUresult r = va_probe(size, &start);
        if (r) return r;
        CUdeviceptr filler = 0;
        if (start < base) {  // pad up to the agreed base
            r = cuMemAddressReserve(&filler, base - start, 0, 0, 0);
            if (r || filler != start) {
                if (!r) cuMemAddressFree(filler, base - start);
                return CUDA_ERROR_INVALID_VALUE;
            }
        }
        CUdeviceptr got = 0;
        r = cuMemAddressReserve(&got, size, 0, base, 0);
        if (!r && got != base) {
            cuMemAddressFree(got, size);
            r = CUDA_ERROR_INVALID_VALUE;
        }
        if (r) {
            if (filler) cuMemAddressFree(filler, base - start);
            return r;
        }
        win_base = base;
        win_size = size;
        logf("device-address window %#llx + %llu GiB", (unsigned long long)base,
             (unsigned long long)(size >> 30));
        return CUDA_SUCCESS;
    }

    CUresult mem_alloc(uint64_t va, uint64_t size) {
        if (!win_size || va < win_base || va + size > win_base + win_size) return CUDA_ERROR_INVALID_VALUE;
        CUresult r;
        CUmemAllocationProp p{};
        p.type = CU_MEM_ALLOCATION_TYPE_PINNED;
        p.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        p.location.id = dev;
        CUmemGenericAllocationHandle h = 0;
        r = cuMemCreate(&h, size, &p, 0);
        if (r) return CUDA_ERROR_OUT_OF_MEMORY;
        r = cuMemMap(va, size, 0, h, 0);
        if (!r) {
            CUmemAccessDesc ad{};
            ad.location = p.location;
            ad.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
            r = cuMemSetAccess(va, size, &ad, 1);
            if (r) cuMemUnmap(va, size);
        }
        if (r) {
            cuMemRelease(h);
            return r;
        }
        allocs[va] = {size, h};
        st.allocs++;
        st.alloc_bytes += size;
        return CUDA_SUCCESS;
    }

    CUresult mem_free(uint64_t va) {
        auto it = allocs.find(va);
        if (it == allocs.end()) return CUDA_ERROR_INVALID_VALUE;
        cuCtxSynchronize();  // same as cudaFree, no kernel may still be using it
        size_t n = it->second.size;
        CUresult r = cuMemUnmap(va, n);
        cuMemRelease(it->second.h);
        st.alloc_bytes -= n;
        allocs.erase(it);
        return r;
    }

    bool htod(const MsgCopy& m) {
        bool ok;
        CUstream s = stream(m.stream, &ok);
        if (!ok) note(CUDA_ERROR_INVALID_HANDLE);
        uint64_t done = 0;
        for (int i = 0; done < m.n; i ^= 1) {
            uint64_t c = std::min<uint64_t>(kChunk, m.n - done);
            cuEventSynchronize(stage_ev[i]);  // wait till the last copy out of this buffer is done
            if (!rd.read(stage[i], c)) return false;
            if (ok) {
                note(cuMemcpyHtoDAsync(m.a + done, stage[i], c, s));
                cuEventRecord(stage_ev[i], s);
            }
            done += c;
        }
        st.bytes_htod += m.n;
        return true;
    }

    // Download. Copies get issued here in stream order, so later work on the
    // stream can't overtake them; tx thread ships each chunk when it's ready.
    // We only block on the first chunk, so an error can still go back instead
    // of the data like it used to.
    bool dtoh(const MsgCopy& m) {
        bool ok;
        CUstream s = stream(m.stream, &ok);
        if (!ok) return reply(CUDA_ERROR_INVALID_HANDLE);
        if (m.n == 0) return reply(sync_result(CUDA_SUCCESS));
        uint64_t off = 0;
        for (int k = 0; off < m.n; k++) {
            uint64_t c = std::min<uint64_t>(kChunk, m.n - off);
            int slot = take_slot();
            if (slot < 0) return false;
            CUresult r = cuMemcpyDtoHAsync(pool[slot], m.a + off, c, s);
            if (!r) r = cuEventRecord(pool_ev[slot], s);
            TxJob j;
            if (k == 0) {
                if (!r) r = cuEventSynchronize(pool_ev[slot]);
                r = sync_result(r);
                if (r) {
                    std::lock_guard<std::mutex> g(txmu);
                    pool_free.push_back(slot);
                    return reply(r);
                }
                ReplyHdr h{0, uint32_t(m.n)};
                j.bytes.assign(reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h) + sizeof h);
                if (m.n <= (64u << 10)) {  // small one: header + data in one write
                    j.bytes.insert(j.bytes.end(), static_cast<uint8_t*>(pool[slot]),
                                   static_cast<uint8_t*>(pool[slot]) + m.n);
                    {
                        std::lock_guard<std::mutex> g(txmu);
                        pool_free.push_back(slot);
                    }
                    st.bytes_dtoh += m.n;
                    return enqueue(std::move(j));
                }
            } else {
                note(r);
            }
            j.slot = slot;
            j.len = c;
            if (!enqueue(std::move(j))) return false;
            off += c;
        }
        st.bytes_dtoh += m.n;
        return true;
    }

    CUresult memset(const MsgMemset& m) {
        bool ok;
        CUstream s = stream(m.stream, &ok);
        if (!ok) return CUDA_ERROR_INVALID_HANDLE;
        if (m.height <= 1) {
            switch (m.elem) {
            case 1: return cuMemsetD8Async(m.dst, uint8_t(m.value), m.width, s);
            case 2: return cuMemsetD16Async(m.dst, uint16_t(m.value), m.width, s);
            default: return cuMemsetD32Async(m.dst, m.value, m.width, s);
            }
        }
        switch (m.elem) {
        case 1: return cuMemsetD2D8Async(m.dst, m.pitch, uint8_t(m.value), m.width, m.height, s);
        case 2: return cuMemsetD2D16Async(m.dst, m.pitch, uint16_t(m.value), m.width, m.height, s);
        default: return cuMemsetD2D32Async(m.dst, m.pitch, m.value, m.width, m.height, s);
        }
    }

    CUresult module_load(const MsgModule& m) {
        std::string path = cache_dir + "/" + hash_name(m.hash);
        std::vector<uint8_t> img;
        if (m.has_data) {
            img.resize(m.n);
            if (m.n && !rd.read(img.data(), m.n)) return CUDA_ERROR_UNKNOWN;
            std::string tmp = path + ".tmp" + std::to_string(getpid());
            int f = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (f >= 0) {
                bool good = write(f, img.data(), img.size()) == ssize_t(img.size());
                close(f);
                if (!good || rename(tmp.c_str(), path.c_str()) != 0) unlink(tmp.c_str());
            }
        } else {
            int f = open(path.c_str(), O_RDONLY);
            struct stat sb{};
            if (f < 0 || fstat(f, &sb) != 0) {
                if (f >= 0) close(f);
                return CUDA_ERROR_FILE_NOT_FOUND;
            }
            img.resize(size_t(sb.st_size));
            bool good = read(f, img.data(), img.size()) == ssize_t(img.size());
            close(f);
            if (!good) return CUDA_ERROR_FILE_NOT_FOUND;
            st.modules_cached++;
        }
        CUmodule mod = nullptr;
        CUresult r = cuModuleLoadData(&mod, img.data());
        if (!r) {
            modules[m.mid] = mod;
            st.modules_loaded++;
        }
        return r;
    }

    CUresult launch(const MsgLaunch& m, uint8_t* args) {
        auto f = funcs.find(m.fid);
        if (f == funcs.end()) return CUDA_ERROR_INVALID_HANDLE;
        bool ok;
        CUstream s = stream(m.stream, &ok);
        if (!ok) return CUDA_ERROR_INVALID_HANDLE;
        size_t sz = m.argsz;
        void* extra[] = {CU_LAUNCH_PARAM_BUFFER_POINTER, args, CU_LAUNCH_PARAM_BUFFER_SIZE, &sz,
                         CU_LAUNCH_PARAM_END};
        st.launches++;
        if (m.flags & 1) {  // cooperative launch wants kernelParams, rebuild from the buffer
            auto L = coop_params.find(m.fid);
            if (L == coop_params.end()) return CUDA_ERROR_NOT_SUPPORTED;
            std::vector<void*> pp;
            for (auto& o : L->second) pp.push_back(args + o);
            return cuLaunchCooperativeKernel(f->second, m.grid[0], m.grid[1], m.grid[2], m.block[0],
                                             m.block[1], m.block[2], m.shmem, s, pp.data());
        }
        return cuLaunchKernel(f->second, m.grid[0], m.grid[1], m.grid[2], m.block[0], m.block[1],
                              m.block[2], m.shmem, s, nullptr, sz ? extra : nullptr);
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> coop_params;  // fid -> param offsets

    CUarray array(uint64_t id) {
        auto it = arrays.find(id);
        return it == arrays.end() ? nullptr : it->second;
    }

    // packed host data <-> 3D copy, see MsgCopy3D
    bool copy3d(const MsgCopy3D& m) {
        CUDA_MEMCPY3D d = m.d;
        bool ok;
        CUstream s = stream(m.stream, &ok);
        std::vector<uint8_t> host;
        CUresult r = ok ? CUDA_SUCCESS : CUDA_ERROR_INVALID_HANDLE;
        size_t W = d.WidthInBytes, H = d.Height ? d.Height : 1, D = d.Depth ? d.Depth : 1;
        if (m.src_host) {
            host.resize(m.packed);
            if (m.packed && !rd.read(host.data(), m.packed)) return false;
            d.srcMemoryType = CU_MEMORYTYPE_HOST;
            d.srcHost = host.data();
            d.srcXInBytes = d.srcY = d.srcZ = 0;
            d.srcPitch = W;
            d.srcHeight = H;
        }
        if (m.dst_host) {
            host.resize(W * H * D);
            d.dstMemoryType = CU_MEMORYTYPE_HOST;
            d.dstHost = host.data();
            d.dstXInBytes = d.dstY = d.dstZ = 0;
            d.dstPitch = W;
            d.dstHeight = H;
        }
        if (d.srcMemoryType == CU_MEMORYTYPE_ARRAY && !(d.srcArray = array(uint64_t(uintptr_t(d.srcArray)))))
            r = CUDA_ERROR_INVALID_HANDLE;
        if (d.dstMemoryType == CU_MEMORYTYPE_ARRAY && !(d.dstArray = array(uint64_t(uintptr_t(d.dstArray)))))
            r = CUDA_ERROR_INVALID_HANDLE;
        if (!r) r = cuMemcpy3DAsync(&d, s);
        if (!r && (m.src_host || m.dst_host)) r = cuStreamSynchronize(s);  // host buffer is ours, wait for it
        if (m.dst_host) {
            r = sync_result(r);
            return r ? reply(r) : reply(CUDA_SUCCESS, host.data(), uint32_t(host.size()));
        }
        note(r);
        return true;
    }

    // map client array ids to CUarrays in a resource desc
    CUresult fix_res(CUDA_RESOURCE_DESC& res) {
        if (res.resType == CU_RESOURCE_TYPE_ARRAY) {
            res.res.array.hArray = array(uint64_t(uintptr_t(res.res.array.hArray)));
            if (!res.res.array.hArray) return CUDA_ERROR_INVALID_HANDLE;
        } else if (res.resType == CU_RESOURCE_TYPE_MIPMAPPED_ARRAY) {
            return CUDA_ERROR_NOT_SUPPORTED;
        }
        return CUDA_SUCCESS;
    }

    void reset() {
        cuCtxSynchronize();
        for (auto t : texs) cuTexObjectDestroy(t);
        for (auto t : surfs) cuSurfObjectDestroy(t);
        for (auto& kv : arrays) cuArrayDestroy(kv.second);
        texs.clear();
        surfs.clear();
        arrays.clear();
        coop_params.clear();
        for (auto& kv : streams) cuStreamDestroy(kv.second);
        for (auto& kv : events) cuEventDestroy(kv.second);
        for (auto& kv : modules) cuModuleUnload(kv.second);
        while (!allocs.empty()) mem_free(allocs.begin()->first);
        streams.clear();
        events.clear();
        modules.clear();
        funcs.clear();
        async_err = CUDA_SUCCESS;
    }

    // handle one message, false means the connection is done
    bool step() {
        MsgHdr h;
        if (!rd.read(&h, sizeof h)) return false;
        if (!ensure()) return false;
        if (!profile) return dispatch(h);
        timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        bool ok = dispatch(h);
        clock_gettime(CLOCK_MONOTONIC, &b);
        auto& p = prof[h.op];
        p.n++;
        p.sec += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
        return ok;
    }

    bool dispatch(const MsgHdr& h) {
        auto take = [&](void* dst, size_t n) { return rd.read(dst, n); };
        switch (h.op) {
        case OP_PING:
            return payload(h.len) && reply(sync_result(CUDA_SUCCESS));
        case OP_MEM_ALLOC: {
            MsgAlloc m;
            if (!take(&m, sizeof m)) return false;
            return reply(mem_alloc(m.va, m.size));
        }
        case OP_MEM_FREE: {
            uint64_t va;
            if (!take(&va, 8)) return false;
            note(mem_free(va));
            return true;
        }
        case OP_MEM_INFO: {
            size_t fr = 0, tot = 0;
            CUresult r = cuMemGetInfo(&fr, &tot);
            uint64_t v[2] = {fr, tot};
            return reply(sync_result(r), v, sizeof v);
        }
        case OP_HTOD: {
            MsgCopy m;
            return take(&m, sizeof m) && htod(m);
        }
        case OP_DTOH: {
            MsgCopy m;
            return take(&m, sizeof m) && dtoh(m);
        }
        case OP_DTOD: {
            MsgCopy m;
            if (!take(&m, sizeof m)) return false;
            bool ok;
            CUstream s = stream(m.stream, &ok);
            note(ok ? cuMemcpyDtoDAsync(m.a, m.b, m.n, s) : CUDA_ERROR_INVALID_HANDLE);
            return true;
        }
        case OP_MEMSET: {
            MsgMemset m;
            if (!take(&m, sizeof m)) return false;
            note(memset(m));
            return true;
        }
        case OP_MODULE_LOAD: {
            MsgModule m;
            if (!take(&m, sizeof m)) return false;
            CUresult r = module_load(m);
            if (r == CUDA_ERROR_UNKNOWN && m.has_data) return false;
            note(r);
            return true;
        }
        case OP_MODULE_UNLOAD: {
            uint32_t mid;
            if (!take(&mid, 4)) return false;
            auto it = modules.find(mid);
            if (it != modules.end()) {
                note(cuModuleUnload(it->second));
                modules.erase(it);
            }
            return true;
        }
        case OP_FUNC_GET: {
            MsgFunc m;
            if (!take(&m, sizeof m) || !payload(m.namelen)) return false;
            std::string name(buf.begin(), buf.end());
            auto it = modules.find(m.mid);
            CUfunction f = nullptr;
            CUresult r = it == modules.end() ? CUDA_ERROR_INVALID_HANDLE
                                             : cuModuleGetFunction(&f, it->second, name.c_str());
            if (!r) funcs[m.fid] = f;
            note(r);
            return true;
        }
        case OP_FUNC_ATTR: {
            MsgFuncAttr m;
            if (!take(&m, sizeof m)) return false;
            auto it = funcs.find(m.fid);
            note(it == funcs.end() ? CUDA_ERROR_INVALID_HANDLE
                                   : cuFuncSetAttribute(it->second, CUfunction_attribute(m.attr), m.value));
            return true;
        }
        case OP_FUNC_CACHE: {
            MsgFuncAttr m;
            if (!take(&m, sizeof m)) return false;
            auto it = funcs.find(m.fid);
            note(it == funcs.end() ? CUDA_ERROR_INVALID_HANDLE
                                   : cuFuncSetCacheConfig(it->second, CUfunc_cache(m.value)));
            return true;
        }
        case OP_LAUNCH: {
            MsgLaunch m;
            if (!take(&m, sizeof m) || !payload(m.argsz)) return false;
            if (m.flags & 1) {  // u32 count + u32 offsets follow
                uint32_t n;
                if (!take(&n, 4)) return false;
                std::vector<uint32_t> off(n);
                if (n && !take(off.data(), n * 4)) return false;
                coop_params[m.fid] = off;
            }
            note(launch(m, buf.data()));
            return true;
        }
        case OP_GLOBAL_GET: {
            MsgGlobal m;
            if (!take(&m, sizeof m) || !payload(m.namelen)) return false;
            std::string name(buf.begin(), buf.end());
            auto it = modules.find(m.mid);
            CUdeviceptr p = 0;
            size_t n = 0;
            CUresult r = it == modules.end() ? CUDA_ERROR_INVALID_HANDLE
                                             : cuModuleGetGlobal(&p, &n, it->second, name.c_str());
            MsgGlobalReply g{p, n};
            return reply(sync_result(r), &g, sizeof g);
        }
        case OP_STREAM_CREATE: {
            MsgStream m;
            if (!take(&m, sizeof m)) return false;
            CUstream s = nullptr;
            CUresult r = cuStreamCreateWithPriority(&s, m.flags, m.prio);
            if (!r) streams[m.sid] = s;
            note(r);
            return true;
        }
        case OP_STREAM_DESTROY: {
            uint64_t sid;
            if (!take(&sid, 8)) return false;
            auto it = streams.find(sid);
            if (it != streams.end()) {
                note(cuStreamDestroy(it->second));
                streams.erase(it);
            }
            return true;
        }
        case OP_STREAM_SYNC:
        case OP_STREAM_QUERY: {
            uint64_t sid;
            if (!take(&sid, 8)) return false;
            bool ok;
            CUstream s = stream(sid, &ok);
            CUresult r = !ok ? CUDA_ERROR_INVALID_HANDLE
                         : h.op == OP_STREAM_SYNC ? cuStreamSynchronize(s) : cuStreamQuery(s);
            if (r == CUDA_ERROR_NOT_READY) return reply(r);  // not really an error
            return reply(sync_result(r));
        }
        case OP_STREAM_WAIT: {
            MsgWait m;
            if (!take(&m, sizeof m)) return false;
            bool ok;
            CUstream s = stream(m.sid, &ok);
            auto e = events.find(m.eid);
            note(!ok || e == events.end() ? CUDA_ERROR_INVALID_HANDLE
                                          : cuStreamWaitEvent(s, e->second, m.flags));
            return true;
        }
        case OP_EVENT_CREATE: {
            MsgEvent m;
            if (!take(&m, sizeof m)) return false;
            CUevent e = nullptr;
            CUresult r = cuEventCreate(&e, m.flags);
            if (!r) events[m.eid] = e;
            note(r);
            return true;
        }
        case OP_EVENT_DESTROY: {
            uint64_t eid;
            if (!take(&eid, 8)) return false;
            auto it = events.find(eid);
            if (it != events.end()) {
                note(cuEventDestroy(it->second));
                events.erase(it);
            }
            return true;
        }
        case OP_EVENT_RECORD: {
            MsgRecord m;
            if (!take(&m, sizeof m)) return false;
            bool ok;
            CUstream s = stream(m.sid, &ok);
            auto e = events.find(m.eid);
            note(!ok || e == events.end() ? CUDA_ERROR_INVALID_HANDLE : cuEventRecord(e->second, s));
            return true;
        }
        case OP_EVENT_SYNC:
        case OP_EVENT_QUERY: {
            uint64_t eid;
            if (!take(&eid, 8)) return false;
            auto e = events.find(eid);
            CUresult r = e == events.end() ? CUDA_ERROR_INVALID_HANDLE
                         : h.op == OP_EVENT_SYNC ? cuEventSynchronize(e->second)
                                                 : cuEventQuery(e->second);
            if (r == CUDA_ERROR_NOT_READY) return reply(r);
            return reply(sync_result(r));
        }
        case OP_EVENT_ELAPSED: {
            MsgElapsed m;
            if (!take(&m, sizeof m)) return false;
            auto a = events.find(m.e0), b = events.find(m.e1);
            float ms = 0;
            CUresult r = a == events.end() || b == events.end()
                             ? CUDA_ERROR_INVALID_HANDLE
                             : cuEventElapsedTime(&ms, a->second, b->second);
            return reply(r, &ms, sizeof ms);
        }
        case OP_CTX_SYNC:
            return payload(h.len) && reply(sync_result(cuCtxSynchronize()));
        case OP_CTX_LIMIT: {
            MsgLimit m;
            if (!take(&m, sizeof m)) return false;
            note(cuCtxSetLimit(CUlimit(m.which), m.value));
            return true;
        }
        case OP_CTX_CACHE: {
            int32_t c;
            if (!take(&c, 4)) return false;
            note(cuCtxSetCacheConfig(CUfunc_cache(c)));
            return true;
        }
        case OP_RESET:
            if (!payload(h.len)) return false;
            reset();
            return reply(CUDA_SUCCESS);
        case OP_STATS:
            return payload(h.len) && reply(CUDA_SUCCESS, &st, sizeof st);
        case OP_VA_PROBE: {
            uint64_t size, where = 0;
            if (!take(&size, 8)) return false;
            CUresult r = va_probe(size, &where);
            return reply(r, &where, 8);
        }
        case OP_ARRAY_CREATE: {
            MsgArray m;
            if (!take(&m, sizeof m)) return false;
            CUarray a = nullptr;
            CUresult r = cuArray3DCreate(&a, &m.desc);
            if (!r) arrays[m.id] = a;
            return reply(sync_result(r));
        }
        case OP_ARRAY_DESTROY: {
            uint64_t id;
            if (!take(&id, 8)) return false;
            auto it = arrays.find(id);
            if (it != arrays.end()) {
                note(cuArrayDestroy(it->second));
                arrays.erase(it);
            }
            return true;
        }
        case OP_COPY3D: {
            MsgCopy3D m;
            return take(&m, sizeof m) && copy3d(m);
        }
        case OP_TEX_CREATE: {
            MsgTex m;
            if (!take(&m, sizeof m)) return false;
            CUtexObject t = 0;
            CUresult r = fix_res(m.res);
            if (!r) r = cuTexObjectCreate(&t, &m.res, &m.tex, m.has_view ? &m.view : nullptr);
            if (!r) texs.insert(t);
            uint64_t v = t;
            return reply(sync_result(r), &v, 8);
        }
        case OP_TEX_DESTROY:
        case OP_SURF_DESTROY: {
            uint64_t v;
            if (!take(&v, 8)) return false;
            if (h.op == OP_TEX_DESTROY && texs.erase(v)) note(cuTexObjectDestroy(v));
            if (h.op == OP_SURF_DESTROY && surfs.erase(v)) note(cuSurfObjectDestroy(v));
            return true;
        }
        case OP_SURF_CREATE: {
            CUDA_RESOURCE_DESC res;
            if (!take(&res, sizeof res)) return false;
            CUsurfObject t = 0;
            CUresult r = fix_res(res);
            if (!r) r = cuSurfObjectCreate(&t, &res);
            if (!r) surfs.insert(t);
            uint64_t v = t;
            return reply(sync_result(r), &v, 8);
        }
        case OP_VA_WINDOW: {
            MsgAlloc m;
            if (!take(&m, sizeof m)) return false;
            return reply(va_window(m.va, m.size));
        }
        default:
            logf("unknown op %u", h.op);
            return false;
        }
    }
};

}  // namespace

static void serve(int fd, const std::string& cache_dir, const char* peer, bool raw) {
    g_raw = raw;
    if (!raw) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    Session s;
    s.fd = fd;
    s.cache_dir = cache_dir;
    if (!g_have_desc || !s.hello()) _exit(1);
    s.profile = getenv("C2C_GPUD_PROFILE") != nullptr;
    logf("client %s connected", peer);
    while (s.step()) {
    }
    for (auto& kv : s.prof)
        logf("  op %2u: %8llu calls, %8.3f s, %7.1f us/call", kv.first, (unsigned long long)kv.second.n,
             kv.second.sec, kv.second.sec / kv.second.n * 1e6);
    logf("client %s gone: %llu launches, %.1f MiB in, %.1f MiB out, %zu allocations still held",
         peer, (unsigned long long)s.st.launches, s.st.bytes_htod / 1048576.0,
         s.st.bytes_dtoh / 1048576.0, s.allocs.size());
    _exit(0);  // driver frees the context and all the memory for us
}

// --- GPU power gating -------------------------------------------------------
// The Jetson GPU power-gates its engines (ELPG) after ~50 us idle and takes
// ~200-350 us to wake back up. Our requests are spaced out by the link round
// trip anyway, so every sync call was eating that wakeup. Fix: turn ELPG off
// while any client is connected, put the old value back 5 s after the last
// one leaves. Runtime sysfs only, needs root, nothing survives a reboot.
static const char* kElpg = "/sys/devices/gpu.0/elpg_enable";
static std::atomic<int> g_clients{0};
static std::string g_elpg_saved;  // set while we have it changed

static bool write_file(const char* path, const std::string& v) {
    int f = open(path, O_WRONLY);
    if (f < 0) return false;
    bool ok = write(f, v.data(), v.size()) == ssize_t(v.size());
    close(f);
    return ok;
}
static std::string read_word(const char* path) {
    char b[32] = {0};
    int f = open(path, O_RDONLY);
    if (f < 0) return "";
    ssize_t n = read(f, b, sizeof b - 1);
    close(f);
    std::string s(b, n > 0 ? size_t(n) : 0);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
static void elpg_hold(bool low_latency) {
    if (!low_latency || !g_elpg_saved.empty()) return;
    std::string cur = read_word(kElpg);
    if (cur.empty() || cur == "0") return;
    if (write_file(kElpg, "0")) {
        g_elpg_saved = cur;
        logf("GPU power gating off while serving (was %s)", cur.c_str());
    } else {
        static bool told = false;
        if (!told) logf("cannot disable GPU power gating (not root?): sync calls pay ~250 us GPU wake-up");
        told = true;
    }
}
static void elpg_release() {
    if (g_elpg_saved.empty()) return;
    if (write_file(kElpg, g_elpg_saved)) logf("GPU power gating restored (%s)", g_elpg_saved.c_str());
    g_elpg_saved.clear();
}
static volatile sig_atomic_t g_child = 0;
static void on_child(int) { g_child = 1; }
static volatile sig_atomic_t g_quit = 0;
static void on_term(int) { g_quit = 1; }

// reads the key = value lines out of /etc/c2c/c2c.conf
static std::map<std::string, std::string> read_conf(const std::string& path) {
    std::map<std::string, std::string> m;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return m;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t h = l.find('#');
        if (h != std::string::npos) l.resize(h);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string x) {
            size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
        };
        m[trim(l.substr(0, eq))] = trim(l.substr(eq + 1));
    }
    std::fclose(f);
    return m;
}

// raw USB channels (/dev/c2cchN), one session per channel at a time
struct Chan {
    int fd = -1;
    pid_t busy = 0;  // pid of the session serving it
};

int main(int argc, char** argv) {
    std::string addr = "10.77.0.2", allow = "10.77.0.1", cache_dir = "/var/cache/c2c-gpud";
    bool low_latency = true, use_raw = true, use_tcp = true;
    int port = kDefaultPort;
    // config file first (/etc/c2c/c2c.conf or --config), cmdline overrides it
    std::string conf_path = "/etc/c2c/c2c.conf";
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--config") conf_path = argv[i + 1];
    {
        auto c = read_conf(conf_path);
        auto get = [&](const char* k) { auto it = c.find(k); return it == c.end() ? std::string() : it->second; };
        if (!get("slave_ip").empty()) addr = get("slave_ip");
        if (!get("master_ip").empty()) allow = get("master_ip");
        if (!get("gpud_spin_us").empty()) g_spin_us = std::atoi(get("gpud_spin_us").c_str());
        if (!get("gpud_download_buffer_mib").empty())
            g_pool_max = std::max(8, std::atoi(get("gpud_download_buffer_mib").c_str()) / int(kChunk >> 20));
        if (get("gpud_keep_power_gating") == "yes") low_latency = false;
        if (get("gpu_transport") == "tcp") use_raw = false;
        if (get("raw_channels") == "0") use_raw = false;
    }
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") next();
        else if (a == "-l") addr = next();
        else if (a == "-p") port = std::atoi(next().c_str());
        else if (a == "-a") allow = next();
        else if (a == "-c") cache_dir = next();
        else if (a == "--spin-us") g_spin_us = std::atoi(next().c_str());
        else if (a == "--download-buffer-mib")
            g_pool_max = std::max(8, std::atoi(next().c_str()) / int(kChunk >> 20));
        else if (a == "--no-raw") use_raw = false;
        else if (a == "--no-tcp") use_tcp = false;
        else if (a == "--keep-power-gating") low_latency = false;
        else {
            std::fprintf(stderr,
                         "usage: c2c-gpud [--config FILE] [-l addr] [-p port] [-a allowed-peer|any] [-c cachedir]\n"
                         "                [--spin-us N] [--download-buffer-mib N] [--no-raw] [--no-tcp]\n"
                         "                [--keep-power-gating]\n"
                         "  serves this GPU on the C2C raw USB channels (/dev/c2cchN) and on TCP\n");
            return 2;
        }
    }
    mkdir(cache_dir.c_str(), 0755);
    struct sigaction sa_chld{}, sa_term{};
    sa_chld.sa_handler = on_child;
    sa_chld.sa_flags = SA_NOCLDSTOP;  // no SA_RESTART on purpose, poll() should wake right away
    sigaction(SIGCHLD, &sa_chld, nullptr);
    sa_term.sa_handler = on_term;
    sigaction(SIGTERM, &sa_term, nullptr);
    sigaction(SIGINT, &sa_term, nullptr);
    signal(SIGPIPE, SIG_IGN);
    int ls = -1;
    if (use_tcp) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(ls, IPPROTO_IP, IP_FREEBIND, &one, sizeof one);  // link might not be up yet
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(uint16_t(port));
        if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1 ||
            bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(ls, 16) != 0) {
            logf("cannot listen on %s:%d: %s", addr.c_str(), port, std::strerror(errno));
            return 1;
        }
    }
    // grab the GPU description once from a helper process. CUDA must NOT get
    // initialised in this one since we fork every session off it
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            pid_t h = fork();
            if (h == 0) {
                close(pfd[0]);
                ServerHello d{};
                CUdevice dv = 0;
                CUresult r = cuInit(0);
                if (!r) r = cuDeviceGet(&dv, 0);
                if (!r) {
                    std::memcpy(d.magic, kHelloServer, 8);
                    d.version = kProtoVersion;
                    cuDriverGetVersion(&d.driver_version);
                    cuDeviceGetName(d.name, sizeof d.name, dv);
                    CUuuid u{};
                    cuDeviceGetUuid(&u, dv);
                    std::memcpy(d.uuid, u.bytes, 16);
                    size_t tot = 0;
                    cuDeviceTotalMem(&tot, dv);
                    d.total_mem = tot;
                    for (int a = 1; a < kMaxAttr; a++) {
                        int v = 0;
                        if (cuDeviceGetAttribute(&v, CUdevice_attribute(a), dv) == CUDA_SUCCESS) d.attr[a] = v;
                    }
                    if (write(pfd[1], &d, sizeof d) != ssize_t(sizeof d)) _exit(1);
                }
                _exit(r ? 1 : 0);
            }
            close(pfd[1]);
            g_have_desc = h > 0 && read(pfd[0], &g_desc, sizeof g_desc) == ssize_t(sizeof g_desc);
            close(pfd[0]);
            if (h > 0) waitpid(h, nullptr, 0);
        }
        if (!g_have_desc) {
            logf("cannot initialise the GPU (CUDA driver missing?)");
            return 1;
        }
        logf("GPU: %s, %.1f GiB", g_desc.name, g_desc.total_mem / 1073741824.0);
    }
    Chan ch[C2C_MAX_CHANNELS];
    std::map<pid_t, int> session_chan;  // pid -> channel idx, -1 for TCP
    auto open_channels = [&]() {
        for (int i = 0; use_raw && i < C2C_MAX_CHANNELS; i++) {
            if (ch[i].fd >= 0) continue;
            char path[32];
            std::snprintf(path, sizeof path, "/dev/c2cch%d", i);
            int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            ch[i].fd = fd;
            logf("serving raw channel %s", path);
        }
    };
    open_channels();
    logf("lending GPU: raw channels %s, TCP %s (clients from %s), module cache %s",
         use_raw ? "on" : "off", use_tcp ? (addr + ":" + std::to_string(port)).c_str() : "off",
         allow.c_str(), cache_dir.c_str());
    auto start_session = [&](int fd, const char* peer, int chan) {
        elpg_hold(low_latency);
        pid_t pid = fork();
        if (pid == 0) {
            if (ls >= 0) close(ls);
            for (auto& c : ch)
                if (c.fd >= 0 && c.fd != fd) close(c.fd);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            serve(fd, cache_dir, peer, chan >= 0);
        }
        if (pid > 0) {
            g_clients++;
            session_chan[pid] = chan;
            if (chan >= 0) ch[chan].busy = pid;
        }
        return pid;
    };
    time_t idle_since = time(nullptr), last_scan = time(nullptr);
    while (!g_quit) {
        // reap dead sessions so their channels free up again
        pid_t pid;
        while ((pid = waitpid(-1, nullptr, WNOHANG)) > 0) {
            g_clients--;
            auto it = session_chan.find(pid);
            if (it != session_chan.end()) {
                if (it->second >= 0) ch[it->second].busy = 0;
                session_chan.erase(it);
            }
        }
        g_child = 0;
        if (time(nullptr) - last_scan >= 2) {  // gadget got re-created, devices come back
            open_channels();
            last_scan = time(nullptr);
        }
        std::vector<pollfd> pf;
        std::vector<int> who;  // -1 is the TCP listener, otherwise channel idx
        if (ls >= 0) {
            pf.push_back({ls, POLLIN, 0});
            who.push_back(-1);
        }
        for (int i = 0; i < C2C_MAX_CHANNELS; i++)
            if (ch[i].fd >= 0 && !ch[i].busy) {
                pf.push_back({ch[i].fd, POLLIN, 0});
                who.push_back(i);
            }
        int pr = poll(pf.data(), pf.size(), 1000);
        if (g_clients.load() > 0) idle_since = time(nullptr);
        else if (!g_elpg_saved.empty() && time(nullptr) - idle_since >= 5) elpg_release();
        if (pr <= 0) continue;
        for (size_t k = 0; k < pf.size(); k++) {
            if (!pf[k].revents) continue;
            if (who[k] < 0) {
                sockaddr_in ca{};
                socklen_t cl = sizeof ca;
                int fd = accept(ls, reinterpret_cast<sockaddr*>(&ca), &cl);
                if (fd < 0) {
                    if (errno != EINTR) usleep(100000);
                    continue;
                }
                char peer[32];
                inet_ntop(AF_INET, &ca.sin_addr, peer, sizeof peer);
                if (allow != "any" && allow != peer) {
                    logf("refused connection from %s", peer);
                    close(fd);
                    continue;
                }
                start_session(fd, peer, -1);
                close(fd);
                continue;
            }
            Chan& c = ch[who[k]];
            if (pf[k].revents & (POLLERR | POLLNVAL)) {  // no USB link at the moment
                if (pf[k].revents & POLLNVAL) {
                    close(c.fd);
                    c.fd = -1;
                }
                continue;
            }
            // new session (or the end of one nobody picked up): accept it
            if (ioctl(c.fd, C2C_IOC_ACCEPT) == 0) {
                char peer[32];
                std::snprintf(peer, sizeof peer, "usb-ch%d", who[k]);
                start_session(c.fd, peer, who[k]);
            } else if (errno == EAGAIN && (pf[k].revents & POLLIN)) {
                // leftovers from a session whose server died, toss them.
                // that app on the master gets no answer and has to be restarted
                char junk[65536];
                while (read(c.fd, junk, sizeof junk) > 0) {
                }
            }
        }
        if (pr > 0 && std::all_of(pf.begin(), pf.end(), [](const pollfd& p) {
                return (p.revents & (POLLERR)) != 0 || !p.revents; }))
            usleep(200000);  // just link errors, don't spin while the cable is out
    }
    elpg_release();
    logf("stopped");
    return 0;
}
