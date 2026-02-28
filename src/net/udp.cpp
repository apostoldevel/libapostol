#include "apostol/udp.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fmt/format.h>

namespace apostol
{

// ─── UdpDatagram ─────────────────────────────────────────────────────────────

std::string UdpDatagram::peer_ip() const
{
    char buf[INET6_ADDRSTRLEN]{};
    if (peer_addr.ss_family == AF_INET) {
        auto* sa = reinterpret_cast<const sockaddr_in*>(&peer_addr);
        ::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    } else if (peer_addr.ss_family == AF_INET6) {
        auto* sa = reinterpret_cast<const sockaddr_in6*>(&peer_addr);
        ::inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf));
    }
    return buf;
}

uint16_t UdpDatagram::peer_port() const
{
    if (peer_addr.ss_family == AF_INET) {
        auto* sa = reinterpret_cast<const sockaddr_in*>(&peer_addr);
        return ntohs(sa->sin_port);
    }
    if (peer_addr.ss_family == AF_INET6) {
        auto* sa = reinterpret_cast<const sockaddr_in6*>(&peer_addr);
        return ntohs(sa->sin6_port);
    }
    return 0;
}

// ─── UdpSocket ───────────────────────────────────────────────────────────────

UdpSocket::UdpSocket(uint16_t port, std::string_view bind_addr)
{
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        throw std::system_error(errno, std::system_category(), "socket(SOCK_DGRAM)");

    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, std::string(bind_addr).c_str(), &addr.sin_addr) != 1) {
        ::close(fd_);
        fd_ = -1;
        throw std::invalid_argument(fmt::format("invalid bind address: {}", bind_addr));
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::system_category(),
            fmt::format("bind({}:{})", bind_addr, port));
    }
}

UdpSocket::~UdpSocket()
{
    if (fd_ >= 0)
        ::close(fd_);
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_)
{
    other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

uint16_t UdpSocket::local_port() const
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        return 0;
    return ntohs(addr.sin_port);
}

std::optional<UdpDatagram> UdpSocket::recv()
{
    // Max UDP datagram size
    static constexpr std::size_t kBufSize = 65536;
    char buf[kBufSize];

    UdpDatagram dgram;
    dgram.peer_len = sizeof(dgram.peer_addr);

    auto n = ::recvfrom(fd_, buf, kBufSize, 0,
                        reinterpret_cast<sockaddr*>(&dgram.peer_addr),
                        &dgram.peer_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::nullopt;
        throw std::system_error(errno, std::system_category(), "recvfrom");
    }

    dgram.data.assign(buf, static_cast<std::size_t>(n));
    return dgram;
}

ssize_t UdpSocket::send(const void* data, std::size_t len,
                          const sockaddr_storage& addr, socklen_t addr_len)
{
    return ::sendto(fd_, data, len, 0,
                    reinterpret_cast<const sockaddr*>(&addr), addr_len);
}

ssize_t UdpSocket::reply(const UdpDatagram& dgram, std::string_view data)
{
    return send(data.data(), data.size(), dgram.peer_addr, dgram.peer_len);
}

} // namespace apostol
