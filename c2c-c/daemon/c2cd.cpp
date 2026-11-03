// c2cd: the link daemon. master or slave depending on /etc/c2c/c2c.conf
//
//   c2cd [-c /etc/c2c/c2c.conf] [-v]
//
// Runs forever, systemd restarts it if it dies. No peer, or a peer with the
// wrong KEY -> we just sit DOWN and nothing else on the box cares. As soon
// as a peer with the right key finishes the handshake the link comes up.
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "link.hpp"
#include "util.hpp"

using namespace c2c;

#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

static void on_term(int) { g_stop = true; }
static void on_usr1(int) {}  // just here to kick blocking syscalls

int main(int argc, char** argv) {
    std::string conf = "/etc/c2c/c2c.conf";
    int opt;
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c': conf = optarg; break;
        case 'v': g_verbose = 1; break;
        default:
            std::fprintf(stderr, "usage: %s [-c config] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    setvbuf(stderr, nullptr, _IOLBF, 0);

    struct sigaction sa{};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    struct sigaction su{};
    su.sa_handler = on_usr1;  // no SA_RESTART so a stuck write() gets EINTR
    sigaction(SIGUSR1, &su, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    std::string err;
    if (!load_config(conf, cfg, err)) {
        LOGE("config: %s", err.c_str());
        return 1;
    }
    std::vector<uint8_t> key;
    if (!load_key(cfg.key_file, key, err)) {
        LOGE("key: %s", err.c_str());
        return 1;
    }
    if (cfg.name.empty()) {
        char hn[64] = {0};
        gethostname(hn, sizeof(hn) - 1);
        cfg.name = hn;
    }
    // remote memory swaps over c2c0, which means through us. If c2cd ever
    // blocks on page reclaim that needs the link we deadlock. So: lock it in
    // RAM and mark allocations non-recursive (all threads inherit this).
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) && mlockall(MCL_CURRENT | MCL_FUTURE))
        LOGI("mlockall failed (%s); continuing", strerror(errno));
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0))
        LOGD("PR_SET_IO_FLUSHER unavailable (%s)", strerror(errno));
    Node node(cfg, std::move(key));
    return node.run();
}
