#include "apostol/tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fmt/format.h>

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

ssize_t TcpConnection::read_drain(std::string& out)
{
    constexpr std::size_t kChunk = 4096;
    ssize_t total = 0;
    char buf[kChunk];

    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            total += n;
            continue;
        }
        if (n == 0)
            return -2;   // peer closed
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return total;
        if (errno == EINTR)
            continue;
        // Other errors (ECONNRESET, EBADF, ...) — treat as closed.
        return -2;
    }
}

ssize_t TcpConnection::write(const void* buf, std::size_t len) noexcept
{
    ssize_t n = ::send(fd_, buf, len, MSG_NOSIGNAL);
    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return -1;   // backpressure — caller buffers + rearms EPOLLOUT
    // EPIPE / ECONNRESET / EBADF / ENOBUFS / … — peer or socket gone.
    return -2;
}

// ─── TcpListener ─────────────────────────────────────────────────────────────

TcpListener::TcpListener(int fd, bool owns) noexcept
    : fd_(fd), backlog_(0), owns_fd_(owns)
{}

TcpListener TcpListener::borrow_fd(int fd)
{
    if (fd < 0)
        throw std::invalid_argument("TcpListener::borrow_fd: invalid fd");
    return TcpListener(fd, false);
}

// Resolve a configured listen address into the sockaddr_in6 a dual-stack socket
// binds. "Every interface" has four spellings in the wild and all four have to keep
// meaning the same thing — APP_DEFAULT_LISTEN is "0.0.0.0", and binding that
// literally as an IPv4-mapped address would cut off IPv6 clients on a socket that
// was deliberately made dual-stack.
static in6_addr resolve_listen_address(std::string_view address)
{
    if (address.empty() || address == "*" || address == "0.0.0.0" || address == "::")
        return in6addr_any;

    const std::string text(address);
    in6_addr out{};

    if (::inet_pton(AF_INET6, text.c_str(), &out) == 1)
        return out;

    in_addr v4{};
    if (::inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        // IPv4-mapped form ::ffff:a.b.c.d — the only way to name an IPv4 interface
        // on an AF_INET6 socket.
        out = in6_addr{};
        out.s6_addr[10] = 0xff;
        out.s6_addr[11] = 0xff;
        std::memcpy(&out.s6_addr[12], &v4.s_addr, sizeof(v4.s_addr));
        return out;
    }

    throw std::invalid_argument(fmt::format("invalid bind address: {}", text));
}

TcpListener::TcpListener(uint16_t port, int backlog, std::string_view address)
    : backlog_(backlog)
{
    // Resolved before the socket exists, so a bad address cannot leave a descriptor
    // behind on the way out.
    const in6_addr bind_addr = resolve_listen_address(address);

    fd_ = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        throw std::system_error(errno, std::system_category(), "socket");

    // Dual-stack (IPv4 + IPv6): disable IPV6_V6ONLY so we listen on both
    int off = 0;
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    // Bind to an address the machine has not been given yet — a DHCP lease, a
    // keepalived VIP, a tunnel that comes up later. Without this the process dies
    // with EADDRNOTAVAIL and, under restart: unless-stopped, keeps dying; on a
    // vessel nobody is reading that log.
    ::setsockopt(fd_, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);
    addr.sin6_addr   = bind_addr;

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::system_category(),
            fmt::format("bind({}:{})", address.empty() ? "*" : address, port));
    }

    if (::listen(fd_, backlog_) < 0) {
        // errno first: close() may overwrite it, and then the diagnostic names the
        // wrong failure. The bind branch above already got this right.
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::system_category(), "listen");
    }
}

TcpListener::~TcpListener()
{
    if (fd_ >= 0 && owns_fd_)
        ::close(fd_);
}

TcpListener::TcpListener(TcpListener&& o) noexcept
    : fd_(o.fd_), backlog_(o.backlog_), owns_fd_(o.owns_fd_)
{
    o.fd_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0 && owns_fd_) ::close(fd_);
        fd_       = o.fd_;
        backlog_  = o.backlog_;
        owns_fd_  = o.owns_fd_;
        o.fd_     = -1;
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

int TcpListener::accept_drain(const AcceptHandler& on_conn)
{
    int count = 0;
    for (;;) {
        std::optional<TcpConnection> conn_opt;
        try {
            conn_opt = accept();
        } catch (const std::system_error&) {
            // EMFILE / ENFILE / ECONNABORTED / EPROTO — swallow so the
            // exception doesn't propagate out of the epoll callback and
            // leave the listener disarmed under ET. The caller will see
            // the accepted count so far; the next readable transition
            // (or descriptor-table recovery) will retry. Without this
            // catch an fd-exhausted worker would go dark until restart.
            return count;
        }
        if (!conn_opt)
            return count;
        if (on_conn)
            on_conn(std::move(*conn_opt));
        ++count;
    }
}

} // namespace apostol
