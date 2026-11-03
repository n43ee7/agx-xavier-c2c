#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "wire.hpp"

namespace c2c {

// ---- logging (stderr -> journald) ----
extern int g_verbose;
#define LOGI(fmt, ...) std::fprintf(stderr, "c2cd: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) std::fprintf(stderr, "c2cd: ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOGD(fmt, ...) do { if (c2c::g_verbose) std::fprintf(stderr, "c2cd: " fmt "\n", ##__VA_ARGS__); } while (0)

uint64_t now_ns();  // CLOCK_MONOTONIC
inline double ns_to_s(uint64_t ns) { return double(ns) / 1e9; }
bool random_bytes(void* buf, size_t len);
std::string hex(const uint8_t* p, size_t n);
bool unhex(const std::string& s, std::vector<uint8_t>& out);
bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data,
                       unsigned mode = 0644);

// ---- config, read from /etc/c2c/c2c.conf ----
struct Config {
    Role role = ROLE_MASTER;
    std::string device = "/dev/c2c0";
    std::string key_file = "/etc/c2c/c2c.key";
    std::string ifname = "c2c0";
    std::string master_ip = "10.77.0.1";
    std::string slave_ip = "10.77.0.2";
    int prefix = 30;
    int mtu = 65000;
    size_t txq_bytes = 4u << 20;      // tx batching queue
    int ping_ms = 500;                // heartbeat period
    int dead_ms = 3000;               // no traffic this long = link lost
    int busy_poll_us = 200;           // spin (no sleep) this long after traffic
    int hello_ms = 1000;              // master retries the handshake this often
    int reject_backoff_ms = 5000;     // wait after a key mismatch
    std::string status_dir = "/run/c2c";
    std::string name;                 // hostname if not set

    const std::string& local_ip() const { return role == ROLE_MASTER ? master_ip : slave_ip; }
    const std::string& peer_ip() const { return role == ROLE_MASTER ? slave_ip : master_ip; }
};

bool load_config(const std::string& path, Config& cfg, std::string& err);
bool load_key(const std::string& path, std::vector<uint8_t>& key, std::string& err);

}  // namespace c2c
