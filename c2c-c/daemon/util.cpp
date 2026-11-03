#include "util.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace c2c {

int g_verbose = 0;

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

bool random_bytes(void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= size_t(n);
    }
    return true;
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

bool unhex(const std::string& s, std::vector<uint8_t>& out) {
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(c));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = v(s[i]), lo = v(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, size_t(n));
    close(fd);
    return n >= 0;
}

bool write_file_atomic(const std::string& path, const std::string& data,
                       unsigned mode) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const char* p = data.data();
    size_t left = data.size();
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += n;
        left -= size_t(n);
    }
    close(fd);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ln++;
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = path + ":" + std::to_string(ln) + ": expected key=value";
            return false;
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        try {
            if (k == "role") {
                if (v == "master") cfg.role = ROLE_MASTER;
                else if (v == "slave") cfg.role = ROLE_SLAVE;
                else { err = "role must be master or slave"; return false; }
            } else if (k == "device") cfg.device = v;
            else if (k == "key_file") cfg.key_file = v;
            else if (k == "ifname") cfg.ifname = v;
            else if (k == "master_ip") cfg.master_ip = v;
            else if (k == "slave_ip") cfg.slave_ip = v;
            else if (k == "prefix") cfg.prefix = std::stoi(v);
            else if (k == "mtu") cfg.mtu = std::stoi(v);
            else if (k == "txq_bytes") cfg.txq_bytes = size_t(std::stoul(v));
            else if (k == "ping_ms") cfg.ping_ms = std::stoi(v);
            else if (k == "dead_ms") cfg.dead_ms = std::stoi(v);
            else if (k == "busy_poll_us") cfg.busy_poll_us = std::stoi(v);
            else if (k == "hello_ms") cfg.hello_ms = std::stoi(v);
            else if (k == "reject_backoff_ms") cfg.reject_backoff_ms = std::stoi(v);
            else if (k == "status_dir") cfg.status_dir = v;
            else if (k == "name") cfg.name = v;
            else if (k == "gadget_mode") { /* c2c-gadget.sh reads this one */ }
            else LOGI("%s:%d: unknown key '%s' ignored", path.c_str(), ln, k.c_str());
        } catch (...) {
            err = path + ":" + std::to_string(ln) + ": bad value for " + k;
            return false;
        }
    }
    if (cfg.mtu < 1280 || cfg.mtu > 65535 - 64) {
        err = "mtu out of range (1280..65471)";
        return false;
    }
    if (cfg.prefix < 8 || cfg.prefix > 30) {
        err = "prefix out of range (8..30)";
        return false;
    }
    if (cfg.txq_bytes < (256u << 10)) cfg.txq_bytes = 256u << 10;
    return true;
}

bool load_key(const std::string& path, std::vector<uint8_t>& key,
              std::string& err) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        err = "key file " + path + " missing";
        return false;
    }
    if (st.st_mode & 077)
        LOGI("warning: %s is readable by group/others (chmod 600)", path.c_str());
    std::string s;
    if (!read_file(path, s)) {
        err = "cannot read " + path;
        return false;
    }
    s = trim(s);
    if (!unhex(s, key) || key.size() < 16) {
        err = path + ": expected >= 32 hex characters";
        return false;
    }
    return true;
}

}  // namespace c2c
