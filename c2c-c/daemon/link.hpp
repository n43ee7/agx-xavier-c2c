// One C2C node. Owns /dev/c2c0, the session (handshake + heartbeat), the IP
// interface and telemetry. Same class for master and slave.
#pragma once
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sysinfo.hpp"
#include "tun.hpp"
#include "util.hpp"
#include "wire.hpp"

namespace c2c {

extern std::atomic<bool> g_stop;

// Everyone appends frames into one flat buffer and the writer pushes it out
// with a single write(), so one USB transfer carries whatever is pending.
// When the link is idle this adds no latency at all.
class TxQueue {
public:
    explicit TxQueue(size_t cap) : cap_(cap) { fill_.reserve(cap); }
    // false = queue closed or timed out
    bool push(uint8_t chan, uint8_t type, const void* a, size_t alen,
              const void* b = nullptr, size_t blen = 0, int timeout_ms = -1);
    bool take(std::vector<uint8_t>& out, int timeout_ms);
    bool wait_data(int timeout_ms);              // true if data pending
    bool take_nowait(std::vector<uint8_t>& out);
    void open();
    void close();
    size_t pending() {
        std::lock_guard<std::mutex> g(m_);
        return fill_.size();
    }

private:
    std::mutex m_;
    std::condition_variable cv_data_, cv_space_;
    std::vector<uint8_t> fill_;
    size_t cap_;
    bool closed_ = true;
};

class Node {
public:
    Node(const Config& cfg, std::vector<uint8_t> key);
    int run();

private:
    enum State { DOWN = 0, HANDSHAKE = 1, UP = 2 };

    void run_connection();
    void rx_loop();
    void tx_loop();
    void tun_loop();
    void housekeeping(uint64_t now);
    void dispatch(const FrameHdr& h, const uint8_t* p);
    void dispatch_ctrl(uint8_t type, const uint8_t* p, uint32_t len);
    bool send_ctrl(uint8_t type, const void* p, size_t len);
    void send_hello();                       // master; caller holds sm_
    void session_up();                       // caller holds sm_
    void session_down(const char* why);      // caller holds sm_
    void kill_connection(const char* why);
    bool write_all(const uint8_t* p, size_t n);  // caller holds wr_mu_
    void write_status(uint64_t now);
    std::string role_str() const { return cfg_.role == ROLE_MASTER ? "master" : "slave"; }

    Config cfg_;
    std::vector<uint8_t> key_;
    Tun tun_;
    TxQueue txq_;
    SysInfoCollector sys_;

    int dev_ = -1;
    pthread_t tx_tid_{};
    pthread_t tun_tid_{};
    std::mutex wr_mu_;  // one write() to the device at a time (queue + fast path)
    std::atomic<bool> conn_dead_{true};
    std::atomic<int> state_{DOWN};
    std::atomic<bool> tx_busy_{false};

    std::mutex sm_;  // guards the session stuff below
    uint8_t nonce_m_[kNonceLen]{};
    uint8_t nonce_s_[kNonceLen]{};
    static constexpr int kOldNonces = 8;    // master: last few HELLO nonces
    uint8_t nonce_old_[kOldNonces][kNonceLen]{};  // so stale replies get ignored
    int nonce_old_pos_ = 0;
    std::string peer_name_;
    std::string last_event_ = "starting";
    SysInfo local_info_{};
    SysInfo peer_info_{};
    bool have_peer_info_ = false;
    uint64_t up_since_ns_ = 0, next_hello_ns_ = 0, last_ping_ns_ = 0,
             last_info_ns_ = 0, last_status_ns_ = 0, last_hreq_ns_ = 0;
    uint64_t last_sample_ns_ = 0, peer_info_ns_ = 0;
    uint64_t last_reject_log_ns_ = 0;
    bool reject_log_due(uint64_t now) {  // log key mismatch at most once a minute
        if (last_reject_log_ns_ && now < last_reject_log_ns_ + 60000000000ull) return false;
        last_reject_log_ns_ = now;
        return true;
    }
    uint64_t ping_seq_ = 0;
    double rtt_us_ = 0, rtt_min_us_ = 0;

    std::atomic<uint64_t> last_rx_ns_{0};
    // busy polling: for busy_poll_us after any traffic the rx and tun loops
    // poll with no sleep, so a reply to something we just sent gets picked
    // up w/o a thread wake-up (saves ~2 x 4 wake-ups per round trip)
    std::atomic<uint64_t> busy_until_ns_{0};
    void note_traffic();
    int poll_timeout(int idle_ms);
    std::atomic<uint64_t> tx_bytes_{0}, rx_bytes_{0}, net_tx_pkts_{0},
        net_rx_pkts_{0}, net_drops_{0}, resync_bytes_{0}, tx_writes_{0};
    uint64_t handshakes_ = 0, losses_ = 0, rejects_ = 0;
    uint64_t prev_tx_ = 0, prev_rx_ = 0, prev_ns_ = 0;
    double tx_rate_ = 0, rx_rate_ = 0;  // bytes/s
};

}  // namespace c2c
