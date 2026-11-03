#include "link.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "c2c_proto.h"
#include "sha256.hpp"

namespace c2c {

std::atomic<bool> g_stop{false};

static constexpr uint64_t MS = 1000000ull;

static void sleep_ms(int ms) {
    while (ms > 0 && !g_stop) {
        int s = ms > 50 ? 50 : ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(s));
        ms -= s;
    }
}

static std::string cstr(const char* p, size_t n) {
    size_t l = strnlen(p, n);
    return std::string(p, l);
}

static std::string json_safe(std::string s) {
    for (char& c : s)
        if (c == '"' || c == '\\' || (unsigned char)c < 0x20) c = '\'';
    return s;
}

// --------------------------------------------------------------------------
// TxQueue

bool TxQueue::push(uint8_t chan, uint8_t type, const void* a, size_t alen,
                   const void* b, size_t blen, int timeout_ms) {
    const size_t need = sizeof(FrameHdr) + alen + blen;
    std::unique_lock<std::mutex> lk(m_);
    auto room = [&] { return closed_ || fill_.empty() || fill_.size() + need <= cap_; };
    if (timeout_ms < 0)
        cv_space_.wait(lk, room);
    else if (!cv_space_.wait_for(lk, std::chrono::milliseconds(timeout_ms), room))
        return false;
    if (closed_) return false;
    const bool was_empty = fill_.empty();
    FrameHdr h = make_hdr(chan, type, uint32_t(alen + blen));
    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&h);
    fill_.insert(fill_.end(), hp, hp + sizeof(h));
    if (alen) fill_.insert(fill_.end(), static_cast<const uint8_t*>(a),
                           static_cast<const uint8_t*>(a) + alen);
    if (blen) fill_.insert(fill_.end(), static_cast<const uint8_t*>(b),
                           static_cast<const uint8_t*>(b) + blen);
    lk.unlock();
    if (was_empty) cv_data_.notify_one();
    return true;
}

bool TxQueue::take(std::vector<uint8_t>& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_data_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return closed_ || !fill_.empty(); }))
        return false;
    if (closed_ || fill_.empty()) return false;
    out.clear();
    std::swap(out, fill_);  // O(1), writer takes the batch and we keep a buffer
    if (fill_.capacity() < cap_) fill_.reserve(cap_);
    lk.unlock();
    cv_space_.notify_all();
    return true;
}

bool TxQueue::wait_data(int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    cv_data_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return closed_ || !fill_.empty(); });
    return !closed_ && !fill_.empty();
}

bool TxQueue::take_nowait(std::vector<uint8_t>& out) {
    std::unique_lock<std::mutex> lk(m_);
    if (closed_ || fill_.empty()) return false;
    out.clear();
    std::swap(out, fill_);
    if (fill_.capacity() < cap_) fill_.reserve(cap_);
    lk.unlock();
    cv_space_.notify_all();
    return true;
}

void TxQueue::open() {
    std::lock_guard<std::mutex> g(m_);
    closed_ = false;
    fill_.clear();
}

void TxQueue::close() {
    {
        std::lock_guard<std::mutex> g(m_);
        closed_ = true;
        fill_.clear();
    }
    cv_data_.notify_all();
    cv_space_.notify_all();
}

// --------------------------------------------------------------------------
// Node

Node::Node(const Config& cfg, std::vector<uint8_t> key)
    : cfg_(cfg), key_(std::move(key)), txq_(cfg.txq_bytes) {}

int Node::run() {
    std::string err;
    if (!tun_.create(cfg_.ifname, cfg_.mtu, cfg_.local_ip(), cfg_.prefix, err)) {
        LOGE("interface %s: %s", cfg_.ifname.c_str(), err.c_str());
        return 1;
    }
    mkdir(cfg_.status_dir.c_str(), 0755);
    LOGI("%s node '%s' started: interface %s %s/%d (peer %s), mtu %d, device %s",
         role_str().c_str(), cfg_.name.c_str(), tun_.name().c_str(),
         cfg_.local_ip().c_str(), cfg_.prefix, cfg_.peer_ip().c_str(), cfg_.mtu,
         cfg_.device.c_str());
    sys_.sample(local_info_);

    std::thread tun_thr(&Node::tun_loop, this);
    tun_tid_ = tun_thr.native_handle();
    std::string last_wait;
    while (!g_stop) {
        std::string wait;
        dev_ = open(cfg_.device.c_str(), O_RDWR | O_CLOEXEC);
        if (dev_ < 0) {
            wait = std::string("waiting for ") + cfg_.device + " (" +
                   strerror(errno) + ")";
        } else if (cfg_.role == ROLE_MASTER) {
            __u32 mode = C2C_MODE_STREAM;
            struct c2c_info info{};
            if (ioctl(dev_, C2C_IOC_SET_MODE, &mode) < 0) {
                wait = std::string("cannot start stream on slave (") +
                       strerror(errno) + ")";
            } else if (ioctl(dev_, C2C_IOC_GET_INFO, &info) == 0) {
                LOGI("USB link: %s, bulk mps %u, burst %u", info.speed_str,
                     info.mps_in, info.maxburst + 1);
            }
        } else {
            struct c2c_gadget_stats gs{};
            if (ioctl(dev_, C2C_IOC_GADGET_STATUS, &gs) < 0)
                wait = std::string("gadget status: ") + strerror(errno);
            else if (gs.speed == 0)
                wait = "waiting for the master's USB host (cable / master powered?)";
        }
        if (!wait.empty()) {
            if (dev_ >= 0) close(dev_);
            dev_ = -1;
            {
                std::lock_guard<std::mutex> g(sm_);
                last_event_ = wait;
            }
            if (wait != last_wait) LOGI("%s", wait.c_str());
            last_wait = wait;
            write_status(now_ns());
            sleep_ms(cfg_.role == ROLE_SLAVE ? 500 : 1000);
            continue;
        }
        last_wait.clear();
        run_connection();
        close(dev_);
        dev_ = -1;
        if (!g_stop) sleep_ms(300);
    }
    tun_.set_up(false);
    tun_thr.join();
    {
        std::lock_guard<std::mutex> g(sm_);
        last_event_ = "daemon stopped";
        state_ = DOWN;
    }
    write_status(now_ns());
    LOGI("stopped");
    return 0;
}

void Node::run_connection() {
    conn_dead_ = false;
    txq_.open();
    {
        std::lock_guard<std::mutex> g(sm_);
        state_ = DOWN;
        next_hello_ns_ = 0;
        last_hreq_ns_ = 0;
        last_event_ = "USB device open, handshaking";
    }
    last_rx_ns_ = now_ns();
    std::thread tx(&Node::tx_loop, this);
    tx_tid_ = tx.native_handle();
    std::thread rx(&Node::rx_loop, this);

    while (!conn_dead_ && !g_stop) {
        housekeeping(now_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_stop && state_ == UP) {
        send_ctrl(BYE, nullptr, 0);
        for (int i = 0; i < 30 && (txq_.pending() || tx_busy_); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    conn_dead_ = true;
    txq_.close();
    pthread_kill(tx_tid_, SIGUSR1);   // unstick a write() blocked in the driver
    pthread_kill(tun_tid_, SIGUSR1);  // (fast path might be writing as well)
    rx.join();
    tx.join();
    std::lock_guard<std::mutex> g(sm_);
    if (state_ != DOWN) session_down("connection closed");
}

void Node::kill_connection(const char* why) {
    if (conn_dead_.exchange(true)) return;
    LOGI("connection closed: %s", why);
    std::lock_guard<std::mutex> g(sm_);
    last_event_ = std::string("connection closed: ") + why;
}

void Node::note_traffic() {
    if (cfg_.busy_poll_us > 0) busy_until_ns_.store(now_ns() + uint64_t(cfg_.busy_poll_us) * 1000);
}

int Node::poll_timeout(int idle_ms) {
    return cfg_.busy_poll_us > 0 && now_ns() < busy_until_ns_.load() ? 0 : idle_ms;
}

void Node::rx_loop() {
    std::vector<uint8_t> buf(4u << 20);
    size_t have = 0;
    pollfd pfd{dev_, POLLIN, 0};
    static const uint8_t magic[4] = {0x43, 0x32, 0x43, 0x31};  // "C2C1"

    while (!conn_dead_ && !g_stop) {
        int r = poll(&pfd, 1, poll_timeout(100));
        if (r < 0) {
            if (errno == EINTR) continue;
            kill_connection("poll failed");
            break;
        }
        if (r == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            kill_connection(cfg_.role == ROLE_SLAVE ? "USB link down"
                                                    : "USB device error/unplugged");
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;
        ssize_t n = read(dev_, buf.data() + have, buf.size() - have);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            kill_connection(strerror(errno));
            break;
        }
        if (n == 0) continue;
        rx_bytes_ += uint64_t(n);
        last_rx_ns_ = now_ns();
        note_traffic();
        have += size_t(n);

        size_t off = 0;
        while (have - off >= sizeof(FrameHdr)) {
            FrameHdr h;
            std::memcpy(&h, buf.data() + off, sizeof(h));
            if (h.magic != kMagic || h.hsum != hdr_sum(h) || h.len > kMaxPayload) {
                const uint8_t* s = buf.data() + off + 1;
                const uint8_t* e = buf.data() + have;
                const void* q = memmem(s, size_t(e - s), magic, 4);
                size_t skip = q ? size_t(static_cast<const uint8_t*>(q) -
                                         (buf.data() + off))
                                : have - off - 3;
                resync_bytes_ += skip;
                off += skip;
                continue;
            }
            if (have - off < sizeof(h) + h.len) break;  // only half a frame so far
            dispatch(h, buf.data() + off + sizeof(h));
            off += sizeof(h) + h.len;
        }
        if (off) {
            std::memmove(buf.data(), buf.data() + off, have - off);
            have -= off;
        }
    }
}

bool Node::write_all(const uint8_t* p, size_t n) {
    size_t off = 0;
    while (off < n && !conn_dead_) {
        ssize_t w = write(dev_, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOTCONN && cfg_.role == ROLE_SLAVE)
                break;  // master hasn't picked STREAM yet, drop it
            kill_connection(strerror(errno));
            break;
        }
        off += size_t(w);
    }
    tx_bytes_ += off;
    tx_writes_++;
    return off == n;
}

void Node::tx_loop() {
    std::vector<uint8_t> batch;
    batch.reserve(cfg_.txq_bytes);
    while (!conn_dead_) {
        if (!txq_.wait_data(100)) continue;
        std::lock_guard<std::mutex> g(wr_mu_);  // only dequeue while holding the lock
        if (!txq_.take_nowait(batch)) continue;
        tx_busy_ = true;
        write_all(batch.data(), batch.size());
        tx_busy_ = false;
    }
}

void Node::tun_loop() {
    // read the packet in after 12 bytes of headroom, then the fast path can
    // drop the frame header in front and do a single write(), no copy
    std::vector<uint8_t> buf(sizeof(FrameHdr) + 65536 + 64);
    uint8_t* pkt = buf.data() + sizeof(FrameHdr);
    const size_t cap = buf.size() - sizeof(FrameHdr);
    pollfd pfd{tun_.fd(), POLLIN, 0};
    while (!g_stop) {
        int r = poll(&pfd, 1, poll_timeout(200));
        if (r <= 0) continue;
        for (int i = 0; i < 64; i++) {  // grab a burst per wakeup
            ssize_t n = read(tun_.fd(), pkt, cap);
            if (n <= 0) break;
            note_traffic();
            if (state_ != UP || conn_dead_) {
                net_drops_++;
                continue;
            }
            // fast path: if the link is idle just write it ourselves (one
            // less thread hop, ~2 wakeups per round trip). Ordering is still
            // fine since the batch writer only dequeues under wr_mu_.
            if (wr_mu_.try_lock()) {
                if (txq_.pending() == 0) {
                    FrameHdr h = make_hdr(CH_NET, 0, uint32_t(n));
                    std::memcpy(buf.data(), &h, sizeof(h));
                    tx_busy_ = true;
                    write_all(buf.data(), sizeof(h) + size_t(n));
                    tx_busy_ = false;
                    wr_mu_.unlock();
                    net_tx_pkts_++;
                    continue;
                }
                wr_mu_.unlock();
            }
            if (txq_.push(CH_NET, 0, pkt, size_t(n), nullptr, 0, 200))
                net_tx_pkts_++;
            else
                net_drops_++;
        }
    }
}

void Node::dispatch(const FrameHdr& h, const uint8_t* p) {
    if (h.chan == CH_NET) {
        if (state_ != UP || h.len <= uint32_t(Tun::kVnetHdr)) {
            net_drops_++;
            return;
        }
        if (write(tun_.fd(), p, h.len) < 0)
            net_drops_++;
        else
            net_rx_pkts_++;
        return;
    }
    if (h.chan == CH_CTRL) dispatch_ctrl(h.type, p, h.len);
}

bool Node::send_ctrl(uint8_t type, const void* p, size_t len) {
    return txq_.push(CH_CTRL, type, p, len, nullptr, 0, 500);
}

void Node::send_hello() {
    std::memcpy(nonce_old_[nonce_old_pos_], nonce_m_, kNonceLen);
    nonce_old_pos_ = (nonce_old_pos_ + 1) % kOldNonces;
    random_bytes(nonce_m_, kNonceLen);
    MsgHello m{};
    m.version = kProtoVersion;
    m.role = ROLE_MASTER;
    std::memcpy(m.nonce, nonce_m_, kNonceLen);
    std::snprintf(m.name, sizeof(m.name), "%s", cfg_.name.c_str());
    send_ctrl(HELLO, &m, sizeof(m));
    state_ = HANDSHAKE;
    next_hello_ns_ = now_ns() + uint64_t(cfg_.hello_ms) * MS;
}

void Node::session_up() {
    state_ = UP;
    up_since_ns_ = now_ns();
    handshakes_++;
    rtt_us_ = rtt_min_us_ = 0;
    have_peer_info_ = false;
    last_info_ns_ = 0;   // send telemetry right away
    last_ping_ns_ = 0;
    tun_.set_up(true);
    last_event_ = "link up with " + peer_name_;
    LOGI("LINK UP: %s (%s) <-> %s (%s), %s %s <-> %s", cfg_.name.c_str(),
         role_str().c_str(), peer_name_.c_str(),
         cfg_.role == ROLE_MASTER ? "slave" : "master", tun_.name().c_str(),
         cfg_.local_ip().c_str(), cfg_.peer_ip().c_str());
}

void Node::session_down(const char* why) {
    if (state_ == UP) {
        losses_++;
        tun_.set_up(false);
        LOGI("LINK DOWN: %s", why);
    }
    state_ = DOWN;
    have_peer_info_ = false;
    last_event_ = why;
}

void Node::dispatch_ctrl(uint8_t type, const uint8_t* p, uint32_t len) {
    std::lock_guard<std::mutex> g(sm_);
    const bool master = cfg_.role == ROLE_MASTER;
    const uint64_t now = now_ns();
    const uint8_t* K = key_.data();
    const size_t KL = key_.size();

    switch (type) {
    case HELLO_REQ:
        if (!master) break;
        if (state_ == UP) {
            session_down("slave restarted its session");
            next_hello_ns_ = now;
        }
        // respect the reject backoff, otherwise answer right away
        if (now >= next_hello_ns_ && now >= last_hreq_ns_ + 200 * MS) {
            last_hreq_ns_ = now;
            send_hello();
        }
        break;

    case HELLO: {
        if (master || len < sizeof(MsgHello)) break;
        MsgHello m;
        std::memcpy(&m, p, sizeof(m));
        if (m.version != kProtoVersion || m.role != ROLE_MASTER) {
            MsgReject r{m.version != kProtoVersion ? REJ_VERSION : REJ_ROLE};
            send_ctrl(REJECT, &r, sizeof(r));
            LOGE("handshake refused: peer version %u role %u", m.version, m.role);
            break;
        }
        if (state_ == UP) session_down("master started a new session");
        std::memcpy(nonce_m_, m.nonce, kNonceLen);
        random_bytes(nonce_s_, kNonceLen);
        peer_name_ = cstr(m.name, sizeof(m.name));
        MsgChallenge c{};
        c.version = kProtoVersion;
        c.role = ROLE_SLAVE;
        std::memcpy(c.nonce, nonce_s_, kNonceLen);
        hmac_sha256(K, KL, "C2C-S", 5, nonce_m_, kNonceLen, nonce_s_, kNonceLen, c.mac);
        std::snprintf(c.name, sizeof(c.name), "%s", cfg_.name.c_str());
        send_ctrl(CHALLENGE, &c, sizeof(c));
        state_ = HANDSHAKE;
        break;
    }

    case CHALLENGE: {
        if (!master || state_ != HANDSHAKE || len < sizeof(MsgChallenge)) break;
        MsgChallenge c;
        std::memcpy(&c, p, sizeof(c));
        std::string pn = cstr(c.name, sizeof(c.name));
        if (c.role != ROLE_SLAVE) {
            MsgReject r{REJ_ROLE};
            send_ctrl(REJECT, &r, sizeof(r));
            LOGE("handshake: peer '%s' is not configured as slave", pn.c_str());
            last_event_ = "peer " + pn + " is not a slave (role conflict)";
            state_ = DOWN;
            next_hello_ns_ = now + uint64_t(cfg_.reject_backoff_ms) * MS;
            break;
        }
        uint8_t mac[kMacLen];
        hmac_sha256(K, KL, "C2C-S", 5, nonce_m_, kNonceLen, c.nonce, kNonceLen, mac);
        if (!ct_equal(mac, c.mac, kMacLen)) {
            bool stale = false;  // is this a valid reply to one of our older HELLOs?
            for (int i = 0; i < kOldNonces && !stale; i++) {
                hmac_sha256(K, KL, "C2C-S", 5, nonce_old_[i], kNonceLen, c.nonce,
                            kNonceLen, mac);
                stale = ct_equal(mac, c.mac, kMacLen);
            }
            if (stale) break;
            MsgReject r{REJ_BAD_KEY};
            send_ctrl(REJECT, &r, sizeof(r));
            rejects_++;
            if (reject_log_due(now))
                LOGE("handshake: slave '%s' presented a different KEY - not "
                     "connecting (retrying every %d ms, logged once a minute)",
                     pn.c_str(), cfg_.reject_backoff_ms);
            last_event_ = "KEY MISMATCH with " + pn + " (C2C inoperable)";
            state_ = DOWN;
            next_hello_ns_ = now + uint64_t(cfg_.reject_backoff_ms) * MS;
            break;
        }
        std::memcpy(nonce_s_, c.nonce, kNonceLen);
        peer_name_ = pn;
        MsgMac a;
        hmac_sha256(K, KL, "C2C-M", 5, nonce_s_, kNonceLen, nonce_m_, kNonceLen, a.mac);
        send_ctrl(AUTH, &a, sizeof(a));
        break;
    }

    case AUTH: {
        if (master || state_ != HANDSHAKE || len < sizeof(MsgMac)) break;
        uint8_t mac[kMacLen];
        hmac_sha256(K, KL, "C2C-M", 5, nonce_s_, kNonceLen, nonce_m_, kNonceLen, mac);
        if (!ct_equal(mac, p, kMacLen)) {
            MsgReject r{REJ_BAD_KEY};
            send_ctrl(REJECT, &r, sizeof(r));
            rejects_++;
            if (reject_log_due(now))
                LOGE("handshake: master '%s' presented a different KEY - not "
                     "connecting (logged once a minute)", peer_name_.c_str());
            last_event_ = "KEY MISMATCH with " + peer_name_ + " (C2C inoperable)";
            state_ = DOWN;
            break;
        }
        MsgMac rd;
        hmac_sha256(K, KL, "C2C-R", 5, nonce_m_, kNonceLen, nonce_s_, kNonceLen, rd.mac);
        send_ctrl(READY, &rd, sizeof(rd));
        session_up();
        break;
    }

    case READY: {
        if (!master || state_ != HANDSHAKE || len < sizeof(MsgMac)) break;
        uint8_t mac[kMacLen];
        hmac_sha256(K, KL, "C2C-R", 5, nonce_m_, kNonceLen, nonce_s_, kNonceLen, mac);
        if (!ct_equal(mac, p, kMacLen)) {
            LOGE("handshake: bad READY from '%s'", peer_name_.c_str());
            state_ = DOWN;
            next_hello_ns_ = now + uint64_t(cfg_.reject_backoff_ms) * MS;
            break;
        }
        session_up();
        break;
    }

    case REJECT: {
        uint32_t reason = 0;
        if (len >= sizeof(MsgReject)) std::memcpy(&reason, p, sizeof(reason));
        const char* why = reason == REJ_BAD_KEY ? "peer rejected our KEY"
                        : reason == REJ_VERSION ? "protocol version mismatch"
                        : reason == REJ_ROLE ? "role conflict (both master/slave?)"
                                             : "peer rejected handshake";
        rejects_++;
        if (reject_log_due(now))
            LOGE("%s - C2C inoperable until keys/roles match (logged once a minute)", why);
        if (!master) last_hreq_ns_ = now + uint64_t(cfg_.reject_backoff_ms) * MS;
        if (state_ == UP) session_down(why);
        state_ = DOWN;
        last_event_ = std::string(why) + " (C2C inoperable)";
        if (master) next_hello_ns_ = now + uint64_t(cfg_.reject_backoff_ms) * MS;
        break;
    }

    case PING:
        if (len >= sizeof(MsgPing)) send_ctrl(PONG, p, sizeof(MsgPing));
        break;

    case PONG:
        if (len >= sizeof(MsgPing)) {
            MsgPing m;
            std::memcpy(&m, p, sizeof(m));
            double rtt = double(now - m.t_ns) / 1e3;
            rtt_us_ = rtt_us_ > 0 ? 0.8 * rtt_us_ + 0.2 * rtt : rtt;
            if (rtt_min_us_ <= 0 || rtt < rtt_min_us_) rtt_min_us_ = rtt;
        }
        break;

    case INFO:
        if (state_ == UP && len >= sizeof(SysInfo)) {
            std::memcpy(&peer_info_, p, sizeof(SysInfo));
            have_peer_info_ = true;
            peer_info_ns_ = now;
        }
        break;

    case BYE:
        if (state_ != DOWN) session_down("peer shut down");
        if (master) next_hello_ns_ = now + uint64_t(cfg_.hello_ms) * MS;
        break;

    default:
        break;
    }
}

void Node::housekeeping(uint64_t now) {
    {
        std::lock_guard<std::mutex> g(sm_);
        const bool master = cfg_.role == ROLE_MASTER;
        if (master && state_ != UP && now >= next_hello_ns_) send_hello();
        if (!master && state_ != UP && now >= last_hreq_ns_ + 2000 * MS) {
            last_hreq_ns_ = now;
            send_ctrl(HELLO_REQ, nullptr, 0);
        }
        if (now >= last_sample_ns_ + 2000 * MS) {
            sys_.sample(local_info_);
            last_sample_ns_ = now;
        }
        if (state_ == UP) {
            if (now > last_rx_ns_ + uint64_t(cfg_.dead_ms) * MS) {
                session_down("heartbeat lost");
                if (master) next_hello_ns_ = now;
            } else {
                if (now >= last_ping_ns_ + uint64_t(cfg_.ping_ms) * MS) {
                    MsgPing m{++ping_seq_, now};
                    send_ctrl(PING, &m, sizeof(m));
                    last_ping_ns_ = now;
                }
                if (now >= last_info_ns_ + 2000 * MS) {
                    send_ctrl(INFO, &local_info_, sizeof(local_info_));
                    last_info_ns_ = now;
                }
            }
        }
    }
    if (now >= last_status_ns_ + 1000 * MS) {
        write_status(now);
        last_status_ns_ = now;
    }
}

void Node::write_status(uint64_t now) {
    uint64_t tx = tx_bytes_, rx = rx_bytes_;
    if (prev_ns_ && now > prev_ns_) {
        double dt = double(now - prev_ns_) / 1e9;
        tx_rate_ = double(tx - prev_tx_) / dt;
        rx_rate_ = double(rx - prev_rx_) / dt;
    }
    prev_tx_ = tx;
    prev_rx_ = rx;
    prev_ns_ = now;

    std::lock_guard<std::mutex> g(sm_);
    const int st = state_;
    const char* sname = st == UP ? "UP" : st == HANDSHAKE ? "HANDSHAKE" : "DOWN";
    const bool master = cfg_.role == ROLE_MASTER;
    uint64_t up_s = st == UP ? (now - up_since_ns_) / 1000000000ull : 0;
    double peer_age = have_peer_info_ ? double(now - peer_info_ns_) / 1e9 : -1;

    char b[1024];
    std::string h;
    std::snprintf(b, sizeof(b), "C2C %s node '%s'   link: %s", role_str().c_str(),
                  cfg_.name.c_str(), sname);
    h += b;
    if (st == UP) {
        std::snprintf(b, sizeof(b), " with '%s' for %lluh%02llum%02llus\n",
                      peer_name_.c_str(), (unsigned long long)(up_s / 3600),
                      (unsigned long long)((up_s / 60) % 60),
                      (unsigned long long)(up_s % 60));
        h += b;
    } else {
        h += "  (C2C inoperable)\n";
    }
    std::snprintf(b, sizeof(b),
                  "  interface : %s  %s/%d -> peer %s (%s)  mtu %d\n"
                  "  traffic   : tx %.3f Gbit/s  rx %.3f Gbit/s   (total tx %.2f GiB, rx %.2f GiB)\n"
                  "  latency   : rtt %.0f us (min %.0f us)\n"
                  "  counters  : handshakes %llu, link losses %llu, rejects %llu, net pkts tx %llu rx %llu, drops %llu, resync %llu B\n"
                  "  last event: %s\n",
                  tun_.name().c_str(), cfg_.local_ip().c_str(), cfg_.prefix,
                  cfg_.peer_ip().c_str(), master ? "c2c-slave" : "c2c-master",
                  cfg_.mtu, tx_rate_ * 8 / 1e9, rx_rate_ * 8 / 1e9,
                  double(tx) / 1073741824.0, double(rx) / 1073741824.0, rtt_us_,
                  rtt_min_us_, (unsigned long long)handshakes_,
                  (unsigned long long)losses_, (unsigned long long)rejects_,
                  (unsigned long long)net_tx_pkts_.load(),
                  (unsigned long long)net_rx_pkts_.load(),
                  (unsigned long long)net_drops_.load(),
                  (unsigned long long)resync_bytes_.load(), last_event_.c_str());
    h += b;
    h += "\nLOCAL (" + role_str() + ")\n" + format_sysinfo(local_info_, "  ");
    if (have_peer_info_) {
        std::snprintf(b, sizeof(b), "\nPEER (%s)   [telemetry age %.1f s]\n",
                      master ? "slave" : "master", peer_age);
        h += b;
        h += format_sysinfo(peer_info_, "  ");
    } else {
        h += "\nPEER: not connected\n";
    }

    std::snprintf(
        b, sizeof(b),
        "{\"role\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"peer\":\"%s\","
        "\"up_s\":%llu,\"ifname\":\"%s\",\"local_ip\":\"%s\",\"peer_ip\":\"%s\","
        "\"mtu\":%d,\"tx_bps\":%.0f,\"rx_bps\":%.0f,\"tx_bytes\":%llu,"
        "\"rx_bytes\":%llu,\"rtt_us\":%.1f,\"rtt_min_us\":%.1f,"
        "\"handshakes\":%llu,\"losses\":%llu,\"rejects\":%llu,"
        "\"net_tx_pkts\":%llu,\"net_rx_pkts\":%llu,\"net_drops\":%llu,"
        "\"resync_bytes\":%llu,\"last_event\":\"%s\",",
        role_str().c_str(), json_safe(cfg_.name).c_str(), sname,
        json_safe(st == UP ? peer_name_ : "").c_str(), (unsigned long long)up_s,
        tun_.name().c_str(), cfg_.local_ip().c_str(), cfg_.peer_ip().c_str(),
        cfg_.mtu, tx_rate_ * 8, rx_rate_ * 8, (unsigned long long)tx,
        (unsigned long long)rx, rtt_us_, rtt_min_us_,
        (unsigned long long)handshakes_, (unsigned long long)losses_,
        (unsigned long long)rejects_, (unsigned long long)net_tx_pkts_.load(),
        (unsigned long long)net_rx_pkts_.load(),
        (unsigned long long)net_drops_.load(),
        (unsigned long long)resync_bytes_.load(),
        json_safe(last_event_).c_str());
    std::string j = b;
    j += "\"local\":" + sysinfo_json(local_info_) + ",\"peer\":" +
         (have_peer_info_ ? sysinfo_json(peer_info_) : std::string("null")) + "}\n";

    write_file_atomic(cfg_.status_dir + "/status", h);
    write_file_atomic(cfg_.status_dir + "/status.json", j);
}

}  // namespace c2c
