#include "apostol/tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace apostol
{

// ─── TcpConnection ────────────────────────────────────────────────────────────

TcpConnection::TcpConnection(int fd, sockaddr_storage peer_addr, socklen_t peer_len) noexcept
    : fd_(fd), peer_addr_(peer_addr), peer_len_(peer_len)
{}

TcpConnection::~TcpConnection()
{
    if (fd_ >= 0)
        ::close(fd_);
}

TcpConnection::TcpConnection(TcpConnection&& o) noexcept
    : fd_(o.fd_), peer_addr_(o.peer_addr_), peer_len_(o.peer_len_)
{
    o.fd_ = -1;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_        = o.fd_;
        peer_addr_ = o.peer_addr_;
        peer_len_  = o.peer_len_;
        o.fd_      = -1;
    }
    return *this;
}

std::string TcpConnection::peer_address() const
{
    char buf[INET6_ADDRSTRLEN] = {};
    const void* src = nullptr;

    if (peer_addr_.ss_family == AF_INET) {
        src = &reinterpret_cast<const sockaddr_in&>(peer_addr_).sin_addr;
    } else {
        src = &reinterpret_cast<const sockaddr_in6&>(peer_addr_).sin6_addr;
    }

    ::inet_ntop(peer_addr_.ss_family, src, buf, sizeof(buf));
    return buf;
}

uint16_t TcpConnection::peer_port() const noexcept
{
    if (peer_addr_.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in&>(peer_addr_).sin_port);
    return ntohs(reinterpret_cast<const sockaddr_in6&>(peer_addr_).sin6_port);
}

ssize_t TcpConnection::read(void* buf, std::size_t len) noexcept
{
    ssize_t n = ::recv(fd_, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -1;
    return n;
}

ssize_t TcpConnection::write(const void* buf, std::size_t len) noexcept
{
    ssize_t n = ::send(fd_, buf, len, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -1;
    return n;
}

// ─── TcpListener ─────────────────────────────────────────────────────────────

TcpListener::TcpListener(uint16_t port, int backlog)
    : backlog_(backlog)
{
    fd_ = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        throw std::system_error(errno, std::system_category(), "socket");

    // Dual-stack (IPv4 + IPv6): disable IPV6_V6ONLY so we listen on both
    int off = 0;
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::system_category(),
            "bind(port=" + std::to_string(port) + ")");
    }

    if (::listen(fd_, backlog_) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(errno, std::system_category(), "listen");
    }
}

TcpListener::~TcpListener()
{
    if (fd_ >= 0)
        ::close(fd_);
}

TcpListener::TcpListener(TcpListener&& o) noexcept : fd_(o.fd_)
{
    o.fd_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_   = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

uint16_t TcpListener::local_port() const
{
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        throw std::system_error(errno, std::system_category(), "getsockname");

    if (addr.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6&>(addr).sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in&>(addr).sin_port);
}

std::optional<TcpConnection> TcpListener::accept()
{
    sockaddr_storage peer{};
    socklen_t len = sizeof(peer);

    int conn_fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::nullopt;
        throw std::system_error(errno, std::system_category(), "accept4");
    }

    // Disable Nagle for lower latency
    int one = 1;
    ::setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return TcpConnection{conn_fd, peer, len};
}

} // namespace apostol
