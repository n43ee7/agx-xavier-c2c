// c2c-memd - lends this board's RAM to the C2C peer (slave side).
//
//   c2c-memd -s 16G [-l 10.77.0.2] [-p 10809] [-S stats_file]
//
// RAM-backed export of the given size. MAP_NORESERVE, so a page only costs
// real RAM once the master writes to it, and TRIM (swap discard) hands it
// back via MADV_DONTNEED. One thread per connection, the kernel NBD client
// pipelines requests on each socket anyway.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rmem_proto.hpp"

using namespace rmem;

static uint8_t* g_mem;
static uint64_t g_size;
static long g_page;
static std::atomic<uint64_t> g_rd_bytes{0}, g_wr_bytes{0}, g_trim_bytes{0}, g_reqs{0};
static std::atomic<int> g_conns{0};
static volatile sig_atomic_t g_stop;

static uint64_t parse_size(const char* s) {
    char* e;
    double v = strtod(s, &e);
    switch (*e) {
    case 'k': case 'K': v *= 1024; break;
    case 'm': case 'M': v *= 1024.0 * 1024; break;
    case 'g': case 'G': v *= 1024.0 * 1024 * 1024; break;
    case 't': case 'T': v *= 1024.0 * 1024 * 1024 * 1024; break;
    }
    return (uint64_t)v & ~(uint64_t)4095;
}

static bool rd_full(int fd, void* p, size_t n) {
    auto* b = (uint8_t*)p;
    while (n) {
        ssize_t r = recv(fd, b, n, MSG_WAITALL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r; n -= r;
    }
    return true;
}

static bool wr_full(int fd, struct iovec* iov, int cnt) {
    while (cnt) {
        ssize_t r = writev(fd, iov, cnt);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        while (cnt && (size_t)r >= iov->iov_len) { r -= iov->iov_len; ++iov; --cnt; }
        if (cnt) { iov->iov_base = (uint8_t*)iov->iov_base + r; iov->iov_len -= r; }
    }
    return true;
}

static void release(uint64_t off, uint64_t len) {   // only the page-aligned part in the middle
    uint64_t end = off + len;
    uint64_t a = (off + g_page - 1) & ~(uint64_t)(g_page - 1);
    uint64_t b = end & ~(uint64_t)(g_page - 1);
    if (b <= a) { memset(g_mem + off, 0, len); return; }
    madvise(g_mem + a, b - a, MADV_DONTNEED);
    if (a > off) memset(g_mem + off, 0, a - off);
    if (end > b) memset(g_mem + b, 0, end - b);
}

static void serve(int fd) {
    // don't set SO_SNDBUF/SO_RCVBUF! it turns off autotuning and the kernel
    // clamps to wmem_max (~416 KiB) -> ~165 MB/s max on a 0.3 ms link. learned that the hard way
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char hello[8];
    if (!rd_full(fd, hello, 8) || memcmp(hello, kHelloClient, 8)) {
        fprintf(stderr, "c2c-memd: bad client hello, closing\n");
        close(fd);
        return;
    }
    ServerHello sh{};
    memcpy(sh.magic, kHelloServer, 8);
    sh.size_be = htobe64(g_size);
    struct iovec hv{&sh, sizeof(sh)};
    if (!wr_full(fd, &hv, 1)) { close(fd); return; }
    int n = ++g_conns;
    fprintf(stderr, "c2c-memd: client connected (%d connection%s)\n", n, n == 1 ? "" : "s");

    std::vector<uint8_t> scratch;
    for (;;) {
        NbdRequest rq;
        if (!rd_full(fd, &rq, sizeof(rq))) break;
        if (ntohl(rq.magic) != kReqMagic) { fprintf(stderr, "c2c-memd: bad request magic\n"); break; }
        uint16_t cmd = ntohl(rq.type) & 0xffff;
        uint64_t from = be64toh(rq.from);
        uint32_t len = ntohl(rq.len);
        NbdReply rp{htonl(kRepMagic), 0, rq.handle};
        bool inb = from <= g_size && len <= g_size - from;
        ++g_reqs;
        if (cmd == CMD_DISC) break;
        switch (cmd) {
        case CMD_READ: {
            if (!inb) { rp.error = htonl(EINVAL); struct iovec v{&rp, sizeof(rp)}; if (!wr_full(fd, &v, 1)) goto out; break; }
            struct iovec v[2] = {{&rp, sizeof(rp)}, {g_mem + from, len}};
            if (!wr_full(fd, v, 2)) goto out;
            g_rd_bytes += len;
            break;
        }
        case CMD_WRITE: {
            void* dst = g_mem + from;
            if (!inb) { scratch.resize(len); dst = scratch.data(); rp.error = htonl(EINVAL); }
            if (!rd_full(fd, dst, len)) goto out;
            if (inb) g_wr_bytes += len;
            struct iovec v{&rp, sizeof(rp)};
            if (!wr_full(fd, &v, 1)) goto out;
            break;
        }
        case CMD_TRIM:
        case CMD_WRITE_ZEROES:
            if (inb) { release(from, len); g_trim_bytes += len; } else rp.error = htonl(EINVAL);
            /* fallthrough */
        case CMD_FLUSH: {
            struct iovec v{&rp, sizeof(rp)};
            if (!wr_full(fd, &v, 1)) goto out;
            break;
        }
        default: {
            rp.error = htonl(EINVAL);
            struct iovec v{&rp, sizeof(rp)};
            if (!wr_full(fd, &v, 1)) goto out;
        }
        }
    }
out:
    close(fd);
    n = --g_conns;
    fprintf(stderr, "c2c-memd: client disconnected (%d left)\n", n);
    if (n == 0) {   // master detached, give everything back
        madvise(g_mem, g_size, MADV_DONTNEED);
        fprintf(stderr, "c2c-memd: all connections closed, memory released\n");
    }
}

static uint64_t resident_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    unsigned long sz = 0, res = 0;
    if (f) { if (fscanf(f, "%lu %lu", &sz, &res) != 2) res = 0; fclose(f); }
    return (uint64_t)res * g_page;
}

static void stats_loop(std::string path) {
    std::string tmp = path + ".tmp";
    while (!g_stop) {
        FILE* f = fopen(tmp.c_str(), "w");
        if (f) {
            fprintf(f, "{\"size\":%llu,\"resident\":%llu,\"read_bytes\":%llu,\"write_bytes\":%llu,"
                       "\"trim_bytes\":%llu,\"requests\":%llu,\"connections\":%d}\n",
                    (unsigned long long)g_size, (unsigned long long)resident_bytes(),
                    (unsigned long long)g_rd_bytes, (unsigned long long)g_wr_bytes,
                    (unsigned long long)g_trim_bytes, (unsigned long long)g_reqs, g_conns.load());
            fclose(f);
            rename(tmp.c_str(), path.c_str());
        }
        usleep(500000);
    }
}

static void on_term(int) { g_stop = 1; }

int main(int argc, char** argv) {
    const char* listen_ip = "10.77.0.2";
    int port = kDefaultPort;
    std::string stats;
    int opt;
    while ((opt = getopt(argc, argv, "s:l:p:S:h")) != -1) {
        switch (opt) {
        case 's': g_size = parse_size(optarg); break;
        case 'l': listen_ip = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'S': stats = optarg; break;
        default:
            fprintf(stderr, "usage: %s -s SIZE[G] [-l listen_ip] [-p port] [-S stats.json]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (!g_size) { fprintf(stderr, "c2c-memd: -s SIZE required\n"); return 2; }
    g_page = sysconf(_SC_PAGESIZE);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_term;   // no SA_RESTART so accept() bails with EINTR
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    g_mem = (uint8_t*)mmap(nullptr, g_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("c2c-memd: mmap"); return 1; }
    madvise(g_mem, g_size, MADV_HUGEPAGE);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, listen_ip, &a.sin_addr) != 1) { fprintf(stderr, "bad ip %s\n", listen_ip); return 2; }
    if (bind(ls, (sockaddr*)&a, sizeof(a)) || listen(ls, 8)) {
        fprintf(stderr, "c2c-memd: bind %s:%d: %s\n", listen_ip, port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "c2c-memd: lending %.2f GiB on %s:%d\n", g_size / 1073741824.0, listen_ip, port);
    if (!stats.empty()) std::thread(stats_loop, stats).detach();

    while (!g_stop) {
        int c = accept(ls, nullptr, nullptr);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        std::thread(serve, c).detach();
    }
    if (!stats.empty()) unlink(stats.c_str());
    fprintf(stderr, "c2c-memd: exiting\n");
    return 0;
}
