// c2c-rmem - hooks the peer's lent RAM (c2c-memd) up as a local block device,
// and optionally as swap. Master side, needs root.
//
//   c2c-rmem attach [-s 10.77.0.2] [-p 10809] [-d /dev/nbd0] [-c conns] [-w PRIO] [-t timeout_s]
//                   [-r readahead_order] [-T]
//   c2c-rmem detach [-d /dev/nbd0]
//   c2c-rmem status [-d /dev/nbd0]
//
// attach -w PRIO = mkswap + swapon --discard -p PRIO, so pages go to the
// peer before any local swap (L4T's zram sits at priority 5).
// The process owning the device (NBD_DO_IT) is mlocked, PR_SET_IO_FLUSHER
// and OOM-exempt. It must never end up waiting on its own swap.
//
// While remote swap is on we tune the kernel for it. Old values are saved and
// put back at detach (-T = don't touch anything):
//   vm.page-cluster=7 + vma_ra_enabled=false: 512 KiB per page-in fault instead
//       of 32 KiB, so each 0.3 ms round trip moves 16x the data. (On 5.10 the
//       per-VMA readahead tops out at 128 KiB, cluster readahead doesn't.)
//   vm.min_free_kbytes>=256 MiB, vm.watermark_scale_factor>=200: kswapd kicks in
//       early, so random tasks rarely end up doing direct reclaim (and network
//       swap-out) themselves. see the kthreadd note in connect_one()
#include <errno.h>
#include <fcntl.h>
#include <linux/nbd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rmem_proto.hpp"

#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

using namespace rmem;

static std::string g_server = "10.77.0.2", g_dev = "/dev/nbd0";
static int g_port = kDefaultPort, g_conns = 2, g_prio = -1, g_timeout = 60, g_ra_order = 7;
static bool g_force = false, g_tune = true, g_vma_ra_off = true;

static std::string devname() { return g_dev.substr(g_dev.rfind('/') + 1); }
static std::string pidfile() { return "/run/c2c/rmem-" + devname() + ".pid"; }
static const char* kTuneFile = "/run/c2c/rmem-vm.saved";

struct Knob { const char* path; long want; bool at_least; };
static const char* kVmaRa = "/sys/kernel/mm/swap/vma_ra_enabled";

static std::string read_str(const char* p) {
    char b[64] = {0};
    FILE* f = fopen(p, "r");
    if (!f) return "";
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
static bool write_str(const char* p, const std::string& v) {
    FILE* f = fopen(p, "w");
    if (!f) return false;
    bool ok = fputs(v.c_str(), f) >= 0;
    return fclose(f) == 0 && ok;
}

// save the current values once (first swap attach), then tune
static void tune_vm() {
    const Knob knobs[] = {{"/proc/sys/vm/page-cluster", g_ra_order, false},
                          {"/proc/sys/vm/min_free_kbytes", 262144, true},
                          {"/proc/sys/vm/watermark_scale_factor", 200, true}};
    if (access(kTuneFile, F_OK) != 0) {
        FILE* f = fopen(kTuneFile, "w");
        if (!f) return;
        for (auto& k : knobs) fprintf(f, "%s %s\n", k.path, read_str(k.path).c_str());
        fprintf(f, "%s %s\n", kVmaRa, read_str(kVmaRa).c_str());
        fclose(f);
    }
    for (auto& k : knobs) {
        long cur = atol(read_str(k.path).c_str());
        if (!k.at_least || cur < k.want) write_str(k.path, std::to_string(k.want));
    }
    if (g_vma_ra_off) write_str(kVmaRa, "false");
    printf("tuned for remote swap: page-cluster %d (%d KiB per fault), min_free_kbytes >= 262144, "
           "watermark_scale_factor >= 200 (restored at detach)\n", g_ra_order, 4 << g_ra_order);
}

// undo tune_vm() once there's no remote swap left
static void restore_vm() {
    FILE* sw = fopen("/proc/swaps", "r");
    if (sw) {
        char line[512];
        bool other = false;
        while (fgets(line, sizeof line, sw))
            if (!strncmp(line, "/dev/nbd", 8) && strncmp(line, g_dev.c_str(), g_dev.size())) other = true;
        fclose(sw);
        if (other) return;  // some other remote swap still wants the tuning
    }
    FILE* f = fopen(kTuneFile, "r");
    if (!f) return;
    char path[256], val[64];
    while (fscanf(f, "%255s %63s", path, val) == 2) write_str(path, val);
    fclose(f);
    unlink(kTuneFile);
    printf("kernel swap settings restored\n");
}

static bool rd_full(int fd, void* p, size_t n) {
    auto* b = (uint8_t*)p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r; n -= r;
    }
    return true;
}

static int connect_one(uint64_t& size) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    inet_pton(AF_INET, g_server.c_str(), &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof(a))) {
        fprintf(stderr, "c2c-rmem: connect %s:%d: %s\n", g_server.c_str(), g_port, strerror(errno));
        close(s);
        return -1;
    }
    // leave SO_SNDBUF/SO_RCVBUF alone: setting them kills autotuning and the
    // kernel clamps to wmem_max (~416 KiB) = ~165 MB/s cap on a 0.3 ms link
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // L4T 5.10 bug: kthreadd does direct reclaim, swaps to this device, TCP
    // send buffer is full -> sk_stream_wait_memory() -> kthread_should_stop()
    // derefs NULL -> panic. (c2c_kfix fixes the root cause.) Belt and braces:
    // size the send buffer above anything the block layer can have in flight
    // (conns x 128 tags x 128 KiB, x2 for skb overhead) so a send never waits.
    int sb = 64 << 20;
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUFFORCE, &sb, sizeof(sb)))
        fprintf(stderr, "c2c-rmem: SO_SNDBUFFORCE failed: %s\n", strerror(errno));
    ServerHello sh;
    if (write(s, kHelloClient, 8) != 8 || !rd_full(s, &sh, sizeof(sh)) ||
        memcmp(sh.magic, kHelloServer, 8)) {
        fprintf(stderr, "c2c-rmem: handshake with %s failed (is c2c-memd running?)\n", g_server.c_str());
        close(s);
        return -1;
    }
    size = be64toh(sh.size_be);
    return s;
}

static long sys_size_bytes() {
    std::string p = "/sys/block/" + devname() + "/size";
    FILE* f = fopen(p.c_str(), "r");
    long v = 0;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
    return v * 512;
}

static bool in_proc_swaps() {
    FILE* f = fopen("/proc/swaps", "r");
    if (!f) return false;
    char line[512];
    bool hit = false;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, g_dev.c_str(), g_dev.size()) && line[g_dev.size()] == ' ') hit = true;
    fclose(f);
    return hit;
}

static int run(const std::string& cmd) {
    int r = system(cmd.c_str());
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static int do_attach() {
    if (sys_size_bytes() > 0) { fprintf(stderr, "c2c-rmem: %s already attached\n", g_dev.c_str()); return 1; }
    if (g_prio >= 0 && access("/sys/module/c2c_kfix", F_OK)) {
        run("modprobe c2c_kfix >/dev/null 2>&1");
        if (access("/sys/module/c2c_kfix", F_OK) && !g_force) {
            fprintf(stderr, "c2c-rmem: refusing swap: c2c_kfix is not loaded. Without it this kernel can\n"
                            "  panic (NULL deref in kthread_should_stop) when swapping over the network.\n"
                            "  Load it (modprobe c2c_kfix) or pass -f to override.\n");
            return 1;
        }
    }
    // nbd needs max_part=0, otherwise every attach/detach triggers a partition
    // rescan on a device that just went away and dmesg fills with scary (but
    // harmless) "I/O error, dev nbdN, sector 0". Only reload if no nbd is busy.
    {
        FILE* f = fopen("/sys/module/nbd/parameters/max_part", "r");
        int mp = 0;
        if (f) { if (fscanf(f, "%d", &mp) != 1) mp = 0; fclose(f); }
        if (mp != 0 && run("for s in /sys/block/nbd*/size; do [ \"$(cat $s)\" = 0 ] || exit 1; done") == 0)
            run("modprobe -r nbd >/dev/null 2>&1");
    }
    run("modprobe nbd nbds_max=4 max_part=0 >/dev/null 2>&1");
    for (int i = 0; i < 50 && access(g_dev.c_str(), F_OK); ++i) usleep(100000);

    std::vector<int> socks;
    uint64_t size = 0;
    for (int i = 0; i < g_conns; ++i) {
        uint64_t sz;
        int s = connect_one(sz);
        if (s < 0) { for (int x : socks) close(x); return 1; }
        if (i && sz != size) { fprintf(stderr, "c2c-rmem: size mismatch\n"); return 1; }
        size = sz;
        socks.push_back(s);
    }

    int ready[2];
    if (pipe(ready)) return 1;
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        // device owner, lives until detach
        close(ready[0]);
        setsid();
        mlockall(MCL_CURRENT | MCL_FUTURE);
        prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
        FILE* oom = fopen("/proc/self/oom_score_adj", "w");
        if (oom) { fputs("-1000", oom); fclose(oom); }
        signal(SIGHUP, SIG_IGN);
        int fd = open(g_dev.c_str(), O_RDWR);
        if (fd < 0) { perror("c2c-rmem: open nbd"); _exit(1); }
        ioctl(fd, NBD_CLEAR_SOCK);
        if (ioctl(fd, NBD_SET_BLKSIZE, 4096UL) || ioctl(fd, NBD_SET_SIZE_BLOCKS, (unsigned long)(size / 4096)) ||
            ioctl(fd, NBD_SET_TIMEOUT, (unsigned long)g_timeout) ||
            ioctl(fd, NBD_SET_FLAGS, (unsigned long)(NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH |
                                                      NBD_FLAG_SEND_TRIM | NBD_FLAG_CAN_MULTI_CONN))) {
            perror("c2c-rmem: nbd setup ioctl");
            _exit(1);
        }
        for (int s : socks)
            if (ioctl(fd, NBD_SET_SOCK, (unsigned long)s)) { perror("c2c-rmem: NBD_SET_SOCK"); _exit(1); }
        FILE* pf = fopen(pidfile().c_str(), "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
        if (write(ready[1], "1", 1) != 1) {}
        close(ready[1]);
        int r = ioctl(fd, NBD_DO_IT);   // blocks until NBD_DISCONNECT or the socket dies
        int e = errno;
        ioctl(fd, NBD_CLEAR_QUE);
        ioctl(fd, NBD_CLEAR_SOCK);
        unlink(pidfile().c_str());
        if (r && e != EPIPE) fprintf(stderr, "c2c-rmem: %s ended: %s\n", g_dev.c_str(), strerror(e));
        _exit(0);
    }
    close(ready[1]);
    for (int s : socks) close(s);
    char c;
    if (read(ready[0], &c, 1) != 1) { fprintf(stderr, "c2c-rmem: device setup failed\n"); return 1; }
    close(ready[0]);
    for (int i = 0; i < 100 && sys_size_bytes() == 0; ++i) usleep(50000);
    if (sys_size_bytes() == 0) { fprintf(stderr, "c2c-rmem: %s did not come up\n", g_dev.c_str()); return 1; }
    // swap writes single pages / small clusters, readahead is useless here
    run("echo none > /sys/block/" + devname() + "/queue/scheduler 2>/dev/null");
    // keeps in-flight bounded, the send buffer sizing above depends on it
    run("echo 128 > /sys/block/" + devname() + "/queue/max_sectors_kb 2>/dev/null");
    printf("attached %s: %.2f GiB from %s:%d (%d connection%s, owner pid %d)\n", g_dev.c_str(),
           size / 1073741824.0, g_server.c_str(), g_port, g_conns, g_conns == 1 ? "" : "s", pid);
    if (g_prio >= 0) {
        if (g_tune) tune_vm();
        if (run("mkswap -L c2c-rmem " + g_dev + " >/dev/null") ||
            run("swapon --discard -p " + std::to_string(g_prio) + " " + g_dev)) {
            fprintf(stderr, "c2c-rmem: mkswap/swapon failed\n");
            return 1;
        }
        printf("swap on %s, priority %d\n", g_dev.c_str(), g_prio);
    }
    return 0;
}

static int do_detach() {
    if (in_proc_swaps()) {
        printf("swapoff %s (pulling remote pages back)...\n", g_dev.c_str());
        if (run("swapoff " + g_dev)) { fprintf(stderr, "c2c-rmem: swapoff failed, not detaching\n"); return 1; }
    }
    restore_vm();
    if (sys_size_bytes() == 0) { printf("%s not attached\n", g_dev.c_str()); return 0; }
    int fd = open(g_dev.c_str(), O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    ioctl(fd, NBD_DISCONNECT);
    ioctl(fd, NBD_CLEAR_SOCK);
    close(fd);
    for (int i = 0; i < 100 && sys_size_bytes() > 0; ++i) usleep(50000);
    printf("detached %s\n", g_dev.c_str());
    return 0;
}

static int do_status() {
    long sz = sys_size_bytes();
    if (!sz) { printf("%s: not attached\n", g_dev.c_str()); return 1; }
    printf("%s: attached, %.2f GiB%s\n", g_dev.c_str(), sz / 1073741824.0, in_proc_swaps() ? ", in use as swap" : "");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
    usage:
        fprintf(stderr,
                "usage: c2c-rmem attach [-s server] [-p port] [-d /dev/nbdN] [-c conns] [-w swap_prio] [-t timeout]\n"
                "                       [-r readahead_order(7)] [-V (keep per-VMA readahead)] [-T (no kernel tuning)] [-f]\n"
                "       c2c-rmem detach [-d /dev/nbdN]\n"
                "       c2c-rmem status [-d /dev/nbdN]\n");
        return 2;
    }
    std::string cmd = argv[1];
    optind = 2;
    int opt;
    while ((opt = getopt(argc, argv, "s:p:d:c:w:t:r:VTf")) != -1) {
        switch (opt) {
        case 's': g_server = optarg; break;
        case 'p': g_port = atoi(optarg); break;
        case 'd': g_dev = optarg; break;
        case 'c': g_conns = atoi(optarg); break;
        case 'w': g_prio = atoi(optarg); break;
        case 't': g_timeout = atoi(optarg); break;
        case 'f': g_force = true; break;
        case 'r': g_ra_order = atoi(optarg); break;
        case 'T': g_tune = false; break;
        case 'V': g_vma_ra_off = false; break;
        default: goto usage;
        }
    }
    mkdir("/run/c2c", 0755);
    if (cmd == "attach") return do_attach();
    if (cmd == "detach") return do_detach();
    if (cmd == "status") return do_status();
    goto usage;
}
