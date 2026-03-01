#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <string>

namespace apostol
{

// ─── TcpConnection ────────────────────────────────────────────────────────────

/// Owns a single non-blocking TCP client socket.
/// Destruction closes the fd.
class TcpConnection
{
public:
    /// Takes ownership of an already-connected fd.
    TcpConnection(int fd, sockaddr_storage peer_addr, socklen_t peer_len) noexcept;

    ~TcpConnection();

    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&&) noexcept;
    TcpConnection& operator=(TcpConnection&&) noexcept;

    int      fd()           const noexcept { return fd_; }
    std::string peer_address() const;   // e.g. "127.0.0.1"
    uint16_t    peer_port()    const noexcept;

    /// Non-blocking read.
    /// Returns: >0 bytes read, 0 on EOF (peer closed), -1 on EAGAIN/EWOULDBLOCK.
    ssize_t read(void* buf, std::size_t len) noexcept;

    /// Non-blocking write.
    /// Returns: bytes written (may be < len), -1 on EAGAIN/EWOULDBLOCK.
    ssize_t write(const void* buf, std::size_t len) noexcept;

private:
    int               fd_       {-1};
    sockaddr_storage  peer_addr_{};
    socklen_t         peer_len_ {0};
};

// ─── TcpListener ─────────────────────────────────────────────────────────────

/// Non-blocking TCP listening socket with SO_REUSEPORT.
/// Destruction closes the fd.
class TcpListener
{
public:
    /// @param port  Local port to bind; 0 = let the OS pick a free port.
    /// @param backlog  listen() backlog (default = APP_DEFAULT_LISTEN_BACKLOG from CMake).
    explicit TcpListener(uint16_t port = 0, int backlog = APP_DEFAULT_LISTEN_BACKLOG);

    /// Wrap an existing bound+listening fd without taking ownership.
    /// The fd will NOT be closed by the destructor — caller (master) manages lifetime.
    static TcpListener borrow_fd(int fd);

    ~TcpListener();

    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;

    int      fd()         const noexcept { return fd_; }
    uint16_t local_port() const;  // via getsockname

    /// Non-blocking accept.
    /// Returns nullopt if there is no pending connection (EAGAIN/EWOULDBLOCK).
    std::optional<TcpConnection> accept();

private:
    explicit TcpListener(int fd, bool owns) noexcept;

    int  fd_       {-1};
    int  backlog_  {APP_DEFAULT_LISTEN_BACKLOG};
    bool owns_fd_  {true};  // false for borrowed fd (worker inherits from master)
};

} // namespace apostol
