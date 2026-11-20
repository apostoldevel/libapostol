#pragma once

#include "apostol/event_loop.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#ifdef WITH_SSL
struct ssl_st;
struct ssl_ctx_st;
#endif

namespace apostol
{

enum class TcpClientState {
    Idle,
    Resolving,
    Connecting,
    TlsHandshake,
    Connected,
    Closing,
    Closed,
    Error
};

// ─── TcpClient ──────────────────────────────────────────────────────────────
//
// Async non-blocking TCP client with EventLoop integration.
//
// Usage:
//   TcpClient client(loop);
//   client.on_connect([]{ fmt::print("connected!\n"); });
//   client.on_data([](const char* d, size_t n){ /* ... */ });
//   client.on_error([](std::string_view e){ /* ... */ });
//   client.connect("example.com", 80);
//
// A handler may destroy the client (its owner lets go of a connection that
// failed or is done): the client touches nothing of itself once the handler
// returns. on_connect, on_close and on_error are called from a copy, so such
// a handler may also set new handlers or connect() again; on_data runs in
// place — after destroying the client it must not use what it captured.
//
class TcpClient
{
public:
    explicit TcpClient(EventLoop& loop);
    ~TcpClient();

    TcpClient(const TcpClient&)            = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&)                 = delete;
    TcpClient& operator=(TcpClient&&)      = delete;

    // ── Connection ──────────────────────────────────────────────────────────

    /// Start async connect to host:port. DNS resolution is synchronous.
    void connect(std::string_view host, uint16_t port);

    /// Enqueue data for sending. EPOLLOUT drives the drain loop.
    void send(std::string_view data);

    /// Initiate graceful close.
    void close();

    // ── State ───────────────────────────────────────────────────────────────

    TcpClientState state() const noexcept { return state_; }
    int  fd() const noexcept { return fd_; }
    bool connected() const noexcept { return state_ == TcpClientState::Connected; }

    // ── Callbacks ───────────────────────────────────────────────────────────

    void on_connect(std::function<void()> cb)                    { on_connect_ = std::move(cb); }
    void on_data(std::function<void(const char*, size_t)> cb)    { on_data_    = std::move(cb); }
    void on_close(std::function<void()> cb)                      { on_close_   = std::move(cb); }
    void on_error(std::function<void(std::string_view)> cb)      { on_error_   = std::move(cb); }

    // ── Timeouts ────────────────────────────────────────────────────────────

    void set_connect_timeout(std::chrono::milliseconds ms) { connect_timeout_ = ms; }
    void set_idle_timeout(std::chrono::milliseconds ms)    { idle_timeout_ = ms; }

    // ── Address check ───────────────────────────────────────────────────────

    /// Called with the resolved address before connect(); a non-empty result
    /// refuses the connection with that message (on_error). Used by HttpClient
    /// for the outbound address filter.
    void set_address_check(std::function<std::string(const struct sockaddr*)> check)
    {
        address_check_ = std::move(check);
    }

    // ── TLS ─────────────────────────────────────────────────────────────────
#ifdef WITH_SSL
    /// Enable TLS. Call before connect() for implicit TLS; it holds for every
    /// later connect() of this client. With verify, the handshake fails unless
    /// the chain leads to the system trust store and the certificate names the
    /// host given to connect() (a DNS name, or an IP address written as one).
    void enable_tls(bool verify = true);

    /// Upgrade an already-connected plain connection to TLS (STARTTLS), with
    /// the verification enable_tls() set (verified by default). The upgrade
    /// is this connection's: the next connect() starts in plain text.
    void start_tls();
#endif

private:
    bool do_resolve(std::string_view host, uint16_t port);
    void do_connect();
    void on_io(uint32_t events);
    void handle_connect_result();
    void on_connected();
    void handle_readable();
    void handle_writable();
    void drain_output();
    void enter_error(std::string_view msg);
    // modify_io() on fd_; false — the loop no longer watches it, and the
    // client is in Error already (on_error_ has run: return at once).
    bool watch(uint32_t events);
    void cleanup();
    void start_connect_timer();
    void cancel_connect_timer();
    void reset_idle_timer();
    void cancel_idle_timer();

#ifdef WITH_SSL
    void do_tls_handshake();
    ssize_t ssl_read(void* buf, size_t len);
    ssize_t ssl_write(const void* buf, size_t len);
#endif

    EventLoop& loop_;
    int fd_{-1};
    bool io_registered_{false};
    TcpClientState state_{TcpClientState::Idle};

    // Resolved address
    struct sockaddr_storage peer_addr_{};
    socklen_t peer_len_{0};
    std::string hostname_;  // saved for TLS SNI

    // Output buffering
    struct WriteChunk {
        std::string data;
        size_t offset{0};  // bytes already sent from this chunk
    };
    std::deque<WriteChunk> output_;

    // Callbacks
    std::function<std::string(const struct sockaddr*)> address_check_;
    std::function<void()>                    on_connect_;
    std::function<void(const char*, size_t)> on_data_;
    std::function<void()>                    on_close_;
    std::function<void(std::string_view)>    on_error_;

    // Timeouts
    std::chrono::milliseconds connect_timeout_{0};
    std::chrono::milliseconds idle_timeout_{0};
    EventLoop::TimerId connect_timer_{EventLoop::kInvalidTimer};
    EventLoop::TimerId idle_timer_{EventLoop::kInvalidTimer};

    // Life token: a handler that destroyed the client expires it, and the
    // code that called the handler returns without touching the client.
    std::shared_ptr<int> alive_{std::make_shared<int>(0)};

    // TLS
#ifdef WITH_SSL
    bool tls_enabled_{false};
    bool tls_verify_{true};
    std::unique_ptr<ssl_ctx_st, void(*)(ssl_ctx_st*)> ssl_ctx_{nullptr, nullptr};
    std::unique_ptr<ssl_st, void(*)(ssl_st*)>         ssl_{nullptr, nullptr};
#endif
};

} // namespace apostol
