#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

    /// Drain the socket's receive buffer into @p out, appending.
    /// Required by the APOSTOL_EPOLL_ET migration: edge-triggered handlers
    /// must read everything available before returning.
    ///
    /// Returns: total bytes appended this call (may be 0 if nothing was
    ///          buffered),
    ///          or -2 if the peer has closed (and any buffered prefix is
    ///          still appended to @p out).
    /// Never returns -1 — EAGAIN is the expected loop-exit condition.
    ssize_t read_drain(std::string& out);

    /// Non-blocking write.
    /// Returns: bytes written (may be < len),
    ///          -1 on EAGAIN/EWOULDBLOCK (backpressure — caller should
    ///          buffer the remainder and rearm EPOLLOUT),
    ///          -2 on fatal error such as EPIPE / ECONNRESET / EBADF
    ///          (peer or socket gone — caller should close, not retry).
    /// Mirrors read_drain's -2 = fatal convention so ET-aware callers can
    /// distinguish backpressure from a dead peer in one check.
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
    /// @param address  Interface to bind. Empty, "*", "0.0.0.0" or "::" mean every
    ///        interface, which is what a dual-stack socket does by default; anything
    ///        else is parsed and bound literally, so a server told to listen on
    ///        127.0.0.1 is reachable only from the host. An address that cannot be
    ///        parsed throws rather than quietly widening to every interface.
    explicit TcpListener(uint16_t port = 0, int backlog = APP_DEFAULT_LISTEN_BACKLOG,
                         std::string_view address = {});

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

    /// Accept every pending connection in a tight loop, invoking @p on_conn
    /// for each. Required by the APOSTOL_EPOLL_ET migration: edge-triggered
    /// listeners receive one event per readable transition, so the handler
    /// must drain the accept queue before returning, otherwise subsequent
    /// pending connections are stranded until the next new connect().
    /// Returns the number of connections accepted (≥ 0).
    using AcceptHandler = std::function<void(TcpConnection)>;
    int accept_drain(const AcceptHandler& on_conn);

private:
    explicit TcpListener(int fd, bool owns) noexcept;

    int  fd_       {-1};
    int  backlog_  {APP_DEFAULT_LISTEN_BACKLOG};
    bool owns_fd_  {true};  // false for borrowed fd (worker inherits from master)
};

} // namespace apostol
