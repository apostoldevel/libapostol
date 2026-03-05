#include "apostol/tcp_client.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <fmt/format.h>

#ifdef WITH_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace apostol
{

// ─── Constructor / Destructor ────────────────────────────────────────────────

TcpClient::TcpClient(EventLoop& loop)
    : loop_(loop)
#ifdef WITH_SSL
    , ssl_ctx_(nullptr, ::SSL_CTX_free)
    , ssl_(nullptr, ::SSL_free)
#endif
{}

TcpClient::~TcpClient()
{
    cleanup();
}

// ─── Public API ──────────────────────────────────────────────────────────────

void TcpClient::connect(std::string_view host, uint16_t port)
{
    if (state_ != TcpClientState::Idle && state_ != TcpClientState::Closed &&
        state_ != TcpClientState::Error)
    {
        enter_error("connect() called in invalid state");
        return;
    }

    // Reset state for reconnect
    cleanup();
    state_ = TcpClientState::Resolving;
    output_.clear();
    hostname_ = std::string(host);

    do_resolve(host, port);
    if (state_ == TcpClientState::Error)
        return;

    do_connect();
}

void TcpClient::send(std::string_view data)
{
    if (data.empty()) return;

    if (state_ != TcpClientState::Connected) {
        // Buffer anyway — will be drained after connect
        if (state_ == TcpClientState::Connecting ||
            state_ == TcpClientState::TlsHandshake)
        {
            output_.push_back({std::string(data), 0});
            return;
        }
        enter_error("send() called on non-connected client");
        return;
    }

    bool was_empty = output_.empty();
    output_.push_back({std::string(data), 0});

    if (was_empty) {
        // Start monitoring EPOLLOUT
        loop_.modify_io(fd_, EPOLLIN | EPOLLOUT);
    }
}

void TcpClient::close()
{
    if (state_ == TcpClientState::Closed || state_ == TcpClientState::Closing)
        return;

    state_ = TcpClientState::Closing;
    cleanup();
    state_ = TcpClientState::Closed;

    if (on_close_)
        on_close_();
}

// ─── DNS Resolution ──────────────────────────────────────────────────────────

void TcpClient::do_resolve(std::string_view host, uint16_t port)
{
    std::string host_str(host);

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    auto port_str = fmt::format("{}", port);

    struct addrinfo* result = nullptr;
    int rc = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        enter_error(fmt::format("getaddrinfo({}): {}", host, gai_strerror(rc)));
        return;
    }

    // Take the first result
    std::memcpy(&peer_addr_, result->ai_addr, result->ai_addrlen);
    peer_len_ = result->ai_addrlen;
    ::freeaddrinfo(result);
}

// ─── Async Connect ───────────────────────────────────────────────────────────

void TcpClient::do_connect()
{
    fd_ = ::socket(peer_addr_.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        enter_error(fmt::format("socket(): {}", std::strerror(errno)));
        return;
    }

    state_ = TcpClientState::Connecting;

    int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&peer_addr_), peer_len_);
    if (rc == 0) {
        // Immediate connect (localhost)
        on_connected();
        return;
    }

    if (errno != EINPROGRESS) {
        enter_error(fmt::format("connect(): {}", std::strerror(errno)));
        return;
    }

    // EINPROGRESS — register for EPOLLOUT to detect completion
    loop_.add_io(fd_, EPOLLOUT, [this](uint32_t events) { on_io(events); });
    io_registered_ = true;

    start_connect_timer();
}

// ─── I/O Dispatch ────────────────────────────────────────────────────────────

void TcpClient::on_io(uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (state_ == TcpClientState::Connecting) {
            handle_connect_result();
        } else {
            enter_error("connection reset by peer");
        }
        return;
    }

    switch (state_) {
        case TcpClientState::Connecting:
            handle_connect_result();
            break;
#ifdef WITH_SSL
        case TcpClientState::TlsHandshake:
            do_tls_handshake();
            break;
#endif
        case TcpClientState::Connected:
            if (events & EPOLLIN)
                handle_readable();
            // handle_readable() callback may have closed/errored the client
            if (state_ == TcpClientState::Connected && (events & EPOLLOUT))
                handle_writable();
            break;
        default:
            break;
    }
}

void TcpClient::handle_connect_result()
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        enter_error(fmt::format("getsockopt(SO_ERROR): {}", std::strerror(errno)));
        return;
    }

    if (err != 0) {
        enter_error(fmt::format("connect failed: {}", std::strerror(err)));
        return;
    }

    cancel_connect_timer();

#ifdef WITH_SSL
    if (tls_enabled_) {
        state_ = TcpClientState::TlsHandshake;
        loop_.modify_io(fd_, EPOLLIN | EPOLLOUT);
        do_tls_handshake();
        return;
    }
#endif

    on_connected();
}

void TcpClient::on_connected()
{
    cancel_connect_timer();
    state_ = TcpClientState::Connected;

    // Switch to EPOLLIN; EPOLLOUT added only when output_ is non-empty
    uint32_t events = EPOLLIN;
    if (!output_.empty())
        events |= EPOLLOUT;

    if (fd_ >= 0) {
        if (io_registered_) {
            loop_.modify_io(fd_, events);
        } else {
            loop_.add_io(fd_, events, [this](uint32_t ev) { on_io(ev); });
            io_registered_ = true;
        }
    }

    reset_idle_timer();

    if (on_connect_)
        on_connect_();
}

void TcpClient::handle_readable()
{
    reset_idle_timer();

    char buf[8192];
    for (;;) {
        ssize_t n;
#ifdef WITH_SSL
        if (ssl_)
            n = ssl_read(buf, sizeof(buf));
        else
#endif
            n = ::recv(fd_, buf, sizeof(buf), 0);

        if (n > 0) {
            if (on_data_)
                on_data_(buf, static_cast<size_t>(n));
            // Callback may have closed/errored this client
            if (state_ != TcpClientState::Connected)
                return;
            continue;
        }

        if (n == 0) {
            // EOF
            close();
            return;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;

        enter_error(fmt::format("recv(): {}", std::strerror(errno)));
        return;
    }
}

void TcpClient::handle_writable()
{
    reset_idle_timer();
    drain_output();
}

void TcpClient::drain_output()
{
    while (!output_.empty()) {
        auto& chunk = output_.front();
        const char* ptr = chunk.data.data() + chunk.offset;
        size_t rem = chunk.data.size() - chunk.offset;

        ssize_t n;
#ifdef WITH_SSL
        if (ssl_)
            n = ssl_write(ptr, rem);
        else
#endif
            n = ::send(fd_, ptr, rem, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;  // will retry on next EPOLLOUT
            enter_error(fmt::format("send(): {}", std::strerror(errno)));
            return;
        }

        chunk.offset += static_cast<size_t>(n);
        if (chunk.offset >= chunk.data.size())
            output_.pop_front();
    }

    // All data sent — stop monitoring EPOLLOUT
    if (fd_ >= 0 && state_ == TcpClientState::Connected)
        loop_.modify_io(fd_, EPOLLIN);
}

// ─── Error handling ──────────────────────────────────────────────────────────

void TcpClient::enter_error(std::string_view msg)
{
    auto prev_state = state_;
    state_ = TcpClientState::Error;
    cleanup();

    if (on_error_)
        on_error_(msg);

    (void)prev_state;
}

void TcpClient::cleanup()
{
    cancel_connect_timer();
    cancel_idle_timer();

    if (fd_ >= 0) {
#ifdef WITH_SSL
        if (ssl_) {
            ::SSL_shutdown(ssl_.get());
            ssl_.reset();
        }
#endif
        if (io_registered_) {
            loop_.remove_io(fd_);
            io_registered_ = false;
        }
        ::close(fd_);
        fd_ = -1;
    }
}

// ─── Timers ──────────────────────────────────────────────────────────────────

void TcpClient::start_connect_timer()
{
    if (connect_timeout_.count() <= 0)
        return;

    connect_timer_ = loop_.add_timer(connect_timeout_, [this] {
        enter_error("connect timeout");
    }, /*repeat=*/false);
}

void TcpClient::cancel_connect_timer()
{
    if (connect_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(connect_timer_);
        connect_timer_ = EventLoop::kInvalidTimer;
    }
}

void TcpClient::reset_idle_timer()
{
    cancel_idle_timer();
    if (idle_timeout_.count() <= 0)
        return;

    idle_timer_ = loop_.add_timer(idle_timeout_, [this] {
        enter_error("idle timeout");
    }, /*repeat=*/false);
}

void TcpClient::cancel_idle_timer()
{
    if (idle_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(idle_timer_);
        idle_timer_ = EventLoop::kInvalidTimer;
    }
}

// ─── TLS ─────────────────────────────────────────────────────────────────────

#ifdef WITH_SSL

void TcpClient::enable_tls(bool verify)
{
    tls_enabled_ = true;
    tls_verify_  = verify;
}

void TcpClient::start_tls()
{
    if (state_ != TcpClientState::Connected) {
        enter_error("start_tls() requires Connected state");
        return;
    }

    tls_enabled_ = true;
    state_ = TcpClientState::TlsHandshake;
    loop_.modify_io(fd_, EPOLLIN | EPOLLOUT);
    do_tls_handshake();
}

void TcpClient::do_tls_handshake()
{
    // Create SSL context and object on first call
    if (!ssl_ctx_) {
        ssl_ctx_.reset(::SSL_CTX_new(::TLS_client_method()));
        if (!ssl_ctx_) {
            enter_error("SSL_CTX_new() failed");
            return;
        }

        if (!tls_verify_) {
            ::SSL_CTX_set_verify(ssl_ctx_.get(), SSL_VERIFY_NONE, nullptr);
        } else {
            ::SSL_CTX_set_default_verify_paths(ssl_ctx_.get());
            ::SSL_CTX_set_verify(ssl_ctx_.get(), SSL_VERIFY_PEER, nullptr);
        }
    }

    if (!ssl_) {
        ssl_.reset(::SSL_new(ssl_ctx_.get()));
        if (!ssl_) {
            enter_error("SSL_new() failed");
            return;
        }
        ::SSL_set_fd(ssl_.get(), fd_);

        // SNI: tell the server which hostname we're connecting to
        if (!hostname_.empty())
            ::SSL_set_tlsext_host_name(ssl_.get(), hostname_.c_str());
    }

    int rc = ::SSL_connect(ssl_.get());
    if (rc == 1) {
        // Handshake complete
        on_connected();
        return;
    }

    int ssl_err = ::SSL_get_error(ssl_.get(), rc);
    switch (ssl_err) {
        case SSL_ERROR_WANT_READ:
            loop_.modify_io(fd_, EPOLLIN);
            break;
        case SSL_ERROR_WANT_WRITE:
            loop_.modify_io(fd_, EPOLLOUT);
            break;
        default: {
            unsigned long e = ::ERR_get_error();
            char buf[256];
            ::ERR_error_string_n(e, buf, sizeof(buf));
            enter_error(fmt::format("SSL_connect(): {}", buf));
            break;
        }
    }
}

ssize_t TcpClient::ssl_read(void* buf, size_t len)
{
    int n = ::SSL_read(ssl_.get(), buf, static_cast<int>(len));
    if (n > 0)
        return n;

    int ssl_err = ::SSL_get_error(ssl_.get(), n);
    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (ssl_err == SSL_ERROR_ZERO_RETURN)
        return 0;  // EOF

    errno = EIO;
    return -1;
}

ssize_t TcpClient::ssl_write(const void* buf, size_t len)
{
    int n = ::SSL_write(ssl_.get(), buf, static_cast<int>(len));
    if (n > 0)
        return n;

    int ssl_err = ::SSL_get_error(ssl_.get(), n);
    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }

    errno = EIO;
    return -1;
}

#endif // WITH_SSL

} // namespace apostol
