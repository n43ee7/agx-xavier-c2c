#include "tun.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace c2c {

Tun::~Tun() {
    if (fd_ >= 0) close(fd_);
    if (sock_ >= 0) close(sock_);
}

static bool set_addr(int sock, const std::string& ifn, unsigned long req,
                     in_addr_t a) {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifn.c_str(), IFNAMSIZ - 1);
    auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = a;
    return ioctl(sock, req, &ifr) == 0;
}

bool Tun::create(const std::string& name, int mtu, const std::string& ip,
                 int prefix, std::string& err) {
    fd_ = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        err = std::string("open /dev/net/tun: ") + strerror(errno);
        return false;
    }
    ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI | IFF_VNET_HDR;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd_, TUNSETIFF, &ifr) < 0) {
        err = std::string("TUNSETIFF: ") + strerror(errno);
        return false;
    }
    name_ = ifr.ifr_name;
    int hdrsz = kVnetHdr;
    if (ioctl(fd_, TUNSETVNETHDRSZ, &hdrsz) < 0) {
        err = std::string("TUNSETVNETHDRSZ: ") + strerror(errno);
        return false;
    }
    // csum offload: the stack gives us CHECKSUM_PARTIAL packets and the peer
    // injects them the same way. USB CRCs every packet anyway.
    if (ioctl(fd_, TUNSETOFFLOAD, TUN_F_CSUM) < 0)
        LOGI("TUNSETOFFLOAD failed (%s), checksums computed in software",
             strerror(errno));

    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        err = std::string("socket: ") + strerror(errno);
        return false;
    }
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) {
        err = "bad ip " + ip;
        return false;
    }
    in_addr_t mask = prefix ? htonl(~((1u << (32 - prefix)) - 1)) : 0;
    if (!set_addr(sock_, name_, SIOCSIFADDR, a.s_addr) ||
        !set_addr(sock_, name_, SIOCSIFNETMASK, mask)) {
        err = std::string("set address: ") + strerror(errno);
        return false;
    }
    ifreq m{};
    std::strncpy(m.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    m.ifr_mtu = mtu;
    if (ioctl(sock_, SIOCSIFMTU, &m) < 0) {
        err = std::string("set mtu: ") + strerror(errno);
        return false;
    }
    ifreq q{};
    std::strncpy(q.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    q.ifr_qlen = 1000;
    ioctl(sock_, SIOCSIFTXQLEN, &q);
    // point to point link, don't want IPv6 autoconf noise
    std::string p = "/proc/sys/net/ipv6/conf/" + name_ + "/disable_ipv6";
    int f = open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (f >= 0) {
        (void)!write(f, "1\n", 2);
        close(f);
    }
    return set_up(false);
}

bool Tun::set_up(bool up) {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    if (ioctl(sock_, SIOCGIFFLAGS, &ifr) < 0) return false;
    if (up)
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    else
        ifr.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
    return ioctl(sock_, SIOCSIFFLAGS, &ifr) == 0;
}

}  // namespace c2c
