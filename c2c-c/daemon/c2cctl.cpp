// c2cctl: poke at the link
//
//   c2cctl status        link + both nodes' resources, readable
//   c2cctl json          same thing as JSON for scripts
//   c2cctl watch [sec]   keep refreshing status (every 1 s by default)
//   c2cctl keygen        spit out a fresh random 256-bit KEY (hex)
//   c2cctl selftest      check the HMAC-SHA256 code
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sha256.hpp"
#include "util.hpp"

using namespace c2c;

static std::string status_dir(const char* conf) {
    Config cfg;
    std::string err;
    if (load_config(conf, cfg, err)) return cfg.status_dir;
    return "/run/c2c";
}

static int show(const std::string& path) {
    std::string s;
    if (!read_file(path, s)) {
        std::fprintf(stderr, "c2cctl: %s not found - is c2cd running? "
                             "(systemctl status c2cd)\n", path.c_str());
        return 1;
    }
    std::fputs(s.c_str(), stdout);
    return 0;
}

static int selftest() {
    // RFC 4231, test case 1
    uint8_t key[20];
    std::memset(key, 0x0b, sizeof(key));
    uint8_t mac[32];
    hmac_sha256(key, sizeof(key), "Hi There", 8, nullptr, 0, nullptr, 0, mac);
    std::string got = hex(mac, 32);
    const char* want =
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    bool ok = got == want;
    std::printf("HMAC-SHA256 RFC4231 #1: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* conf = "/etc/c2c/c2c.conf";
    std::string cmd = argc > 1 ? argv[1] : "status";
    std::string dir = status_dir(conf);

    if (cmd == "status") return show(dir + "/status");
    if (cmd == "json") return show(dir + "/status.json");
    if (cmd == "watch") {
        int sec = argc > 2 ? std::atoi(argv[2]) : 1;
        if (sec < 1) sec = 1;
        for (;;) {
            std::printf("\033[H\033[2J");
            show(dir + "/status");
            std::fflush(stdout);
            sleep(unsigned(sec));
        }
    }
    if (cmd == "keygen") {
        uint8_t k[32];
        if (!random_bytes(k, sizeof(k))) return 1;
        std::printf("%s\n", hex(k, sizeof(k)).c_str());
        return 0;
    }
    if (cmd == "selftest") return selftest();
    std::fprintf(stderr, "usage: c2cctl status|json|watch [sec]|keygen|selftest\n");
    return 2;
}
