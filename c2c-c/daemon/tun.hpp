// The IP side of the link: TUN device w/ virtio-net headers + csum offload.
// Lives as long as the daemon, but is only set UP while there's an
// authenticated session.
#pragma once
#include <string>

namespace c2c {

class Tun {
public:
    ~Tun();
    bool create(const std::string& name, int mtu, const std::string& ip,
                int prefix, std::string& err);
    bool set_up(bool up);
    int fd() const { return fd_; }
    const std::string& name() const { return name_; }
    static constexpr int kVnetHdr = 10;  // sizeof(struct virtio_net_hdr)

private:
    int fd_ = -1;
    int sock_ = -1;
    std::string name_;
};

}  // namespace c2c
