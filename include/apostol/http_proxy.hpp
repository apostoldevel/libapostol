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

    /// Forward an incoming request to the upstream and call send_response with the result.
    void forward(const HttpRequest& req, SendResponse send_response,
                 std::function<void(std::string_view)> on_error = {});

    void set_timeout(std::chrono::milliseconds ms) { timeout_ = ms; }

#ifdef WITH_SSL
    void set_tls(bool enable = true) { tls_enabled_ = enable; }
#endif

private:
    struct ForwardCtx;

    EventLoop& loop_;
    std::string upstream_host_;
    uint16_t    upstream_port_;
    std::chrono::milliseconds timeout_{30000};

    std::vector<std::unique_ptr<ForwardCtx>> contexts_;
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
