#pragma once

#include "apostol/event_loop.hpp"
#include "apostol/http.hpp"
#include "apostol/tcp_client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── HttpProxy ──────────────────────────────────────────────────────────────
//
// Bidirectional HTTP relay: receives a server-side HttpRequest, forwards it
// to an upstream via TcpClient, and returns the upstream response.
//
// Usage:
//   HttpProxy proxy(loop, "backend.local", 8080);
//   proxy.forward(req, [](const HttpResponse& resp) { /* send to client */ });
//
class HttpProxy
{
public:
    using SendResponse = std::function<void(const HttpResponse&)>;

    HttpProxy(EventLoop& loop, std::string_view upstream_host, uint16_t upstream_port);
    ~HttpProxy();

    HttpProxy(const HttpProxy&)            = delete;
    HttpProxy& operator=(const HttpProxy&) = delete;

    /// Why a forward produced no upstream response.
    struct ForwardFailure
    {
        enum class Reason
        {
            connect,           // nothing reached the upstream: refused, unreachable, connect timeout, TLS
            idle_timeout,      // set_timeout() elapsed with no byte from the upstream
            response_timeout,  // set_response_timeout() elapsed before the last byte
            closed,            // upstream closed the connection before a full response
            other,             // any other socket error after the request went out
        };
        Reason           reason;
        std::string_view message;   // TcpClient's text, for the log — classify by reason, not by it
        /// True once the request was handed to the connected socket. A caller
        /// that retries on another upstream must not do so after this point:
        /// the first upstream may have executed the request. Equivalent to
        /// reason != Reason::connect.
        bool request_sent;
    };
    using OnFailure = std::function<void(const ForwardFailure&)>;

    /// Forward an incoming request to the upstream and call send_response with the result.
    /// Without on_error a failure is answered 502 through send_response.
    void forward(const HttpRequest& req, SendResponse send_response,
                 std::function<void(std::string_view)> on_error = {});

    /// Same, with the failure reported as a ForwardFailure — for a caller
    /// that has to know whether a retry is still safe.
    void forward(const HttpRequest& req, SendResponse send_response, OnFailure on_failure);

    /// Idle timeout of the upstream connection — TcpClient's idle timer, re-armed
    /// by every byte received, not a limit on the whole exchange. Unless
    /// set_connect_timeout() was called it bounds the connect as well.
    void set_timeout(std::chrono::milliseconds ms) { timeout_ = ms; }

    /// Deadline on the whole answer: from the request being handed to the
    /// socket to the last byte of the response. The idle timer above never
    /// fires on an upstream that keeps trickling bytes; this one does. Zero —
    /// the default — disables it. While it is set the idle timer is armed with
    /// max(set_timeout(), this), so a silent upstream is reported at whichever
    /// is earlier and a deadline longer than the idle default (30 s) is reachable.
    void set_response_timeout(std::chrono::milliseconds ms) { response_timeout_ = ms; }

    /// Connect timeout on its own. A proxy that may retry elsewhere wants a
    /// short connect (nothing has been sent yet, so a retry is safe) and a long
    /// response wait; one knob for both left one of them wrong. Zero — the
    /// default — keeps the old behaviour: connect is bounded by set_timeout().
    void set_connect_timeout(std::chrono::milliseconds ms) { connect_timeout_ = ms; }

    /// Whether forward() appends "X-Forwarded-For: <peer_ip>" to the upstream
    /// request (on by default). A caller sitting behind an outer proxy already
    /// carries the client's address in that header and wants exactly one:
    /// the address this proxy would add is the inner hop, not the client.
    void set_append_forwarded_for(bool on) { append_forwarded_for_ = on; }

#ifdef WITH_SSL
    void set_tls(bool enable = true) { tls_enabled_ = enable; }
#endif

private:
    struct ForwardCtx;

    EventLoop& loop_;
    std::string upstream_host_;
    uint16_t    upstream_port_;
    std::chrono::milliseconds timeout_{30000};
    std::chrono::milliseconds connect_timeout_{0};
    std::chrono::milliseconds response_timeout_{0};
    bool append_forwarded_for_{true};

    void forward_impl(const HttpRequest& req, SendResponse send_response, OnFailure on_failure);

    std::vector<std::unique_ptr<ForwardCtx>> contexts_;
    // The deferred sweep of settled contexts: one pending at a time. Its timer
    // callback holds a weak_ptr to this token, not this — the proxy may be
    // destroyed in the same tick its last forward settles, or as a module
    // member in ~Application, after the EventLoop it was armed on is gone;
    // so the destructor never touches the loop, the stale callback just
    // finds the token expired.
    std::shared_ptr<int> alive_{std::make_shared<int>(0)};
    EventLoop::TimerId cleanup_timer_{EventLoop::kInvalidTimer};
    void schedule_cleanup();
    void cleanup_done();

#ifdef WITH_SSL
    bool tls_enabled_{false};
#endif
};

// ─── HttpProxyManager ───────────────────────────────────────────────────────

/// Manages multiple HttpProxy instances keyed by upstream target.
class HttpProxyManager
{
public:
    explicit HttpProxyManager(EventLoop& loop);

    HttpProxy& add(std::string_view host, uint16_t port);
    void cleanup();

private:
    EventLoop& loop_;
    std::vector<std::unique_ptr<HttpProxy>> proxies_;
};

} // namespace apostol
