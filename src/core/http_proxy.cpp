#include "apostol/http_proxy.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace apostol
{

// ─── ForwardCtx ──────────────────────────────────────────────────────────────

struct HttpProxy::ForwardCtx
{
    TcpClient           tcp;
    HttpResponseParser  parser;
    SendResponse        send_response;
    std::function<void(std::string_view)> on_error;
    bool                done{false};

    explicit ForwardCtx(EventLoop& loop) : tcp(loop) {}
};

// ─── HttpProxy ───────────────────────────────────────────────────────────────

HttpProxy::HttpProxy(EventLoop& loop, std::string_view upstream_host, uint16_t upstream_port)
    : loop_(loop)
    , upstream_host_(upstream_host)
    , upstream_port_(upstream_port)
{}

HttpProxy::~HttpProxy() = default;

void HttpProxy::forward(const HttpRequest& req, SendResponse send_response,
                         std::function<void(std::string_view)> on_error)
{
    auto ctx = std::make_unique<ForwardCtx>(loop_);
    auto* ptr = ctx.get();

    ptr->send_response = std::move(send_response);
    ptr->on_error      = std::move(on_error);

    if (timeout_.count() > 0) {
        ptr->tcp.set_connect_timeout(timeout_);
        ptr->tcp.set_idle_timeout(timeout_);
    }

#ifdef WITH_SSL
    if (tls_enabled_)
        ptr->tcp.enable_tls(false);
#endif

    // Serialize the request for the upstream
    std::string serialized;
    serialized.reserve(256 + req.body.size());

    // Reconstruct request line
    std::string path = req.path;
    if (!req.query.empty())
        path += "?" + req.query;

    serialized += fmt::format("{} {} HTTP/1.1\r\n", req.method, path);
    serialized += fmt::format("Host: {}\r\n", upstream_host_);

    // Forward headers (skip Host — we already set it)
    bool has_content_length = false;
    for (const auto& [k, v] : req.headers) {
        std::string lower_k = k;
        std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_k == "host") continue;
        if (lower_k == "content-length") has_content_length = true;
        serialized += fmt::format("{}: {}\r\n", k, v);
    }

    if (!has_content_length && !req.body.empty())
        serialized += fmt::format("Content-Length: {}\r\n", req.body.size());

    // Add X-Forwarded-For
    if (!req.peer_ip.empty())
        serialized += fmt::format("X-Forwarded-For: {}\r\n", req.peer_ip);

    serialized += "Connection: close\r\n\r\n";
    serialized += req.body;

    // Set up response parsing
    ptr->parser.set_handler([ptr, this](HttpClientResponse upstream_resp) {
        if (ptr->done) return;
        ptr->done = true;

        // Build server-side HttpResponse from upstream response
        HttpResponse resp;
        resp.set_status(upstream_resp.status_code, upstream_resp.status_text);

        for (const auto& [k, v] : upstream_resp.headers) {
            // Skip hop-by-hop headers
            std::string lower_k = k;
            std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower_k == "transfer-encoding" || lower_k == "connection")
                continue;
            resp.add_header(k, v);
        }

        if (!upstream_resp.body.empty())
            resp.set_body(std::move(upstream_resp.body));

        if (ptr->send_response)
            ptr->send_response(resp);

        ptr->tcp.close();
        // Defer cleanup — we're inside TcpClient's on_data callback chain
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    });

    ptr->tcp.on_connect([ptr, data = std::move(serialized)] {
        ptr->tcp.send(data);
    });

    ptr->tcp.on_data([ptr](const char* data, size_t len) {
        if (!ptr->done)
            ptr->parser.feed(data, len);
    });

    auto error_handler = [ptr, this](std::string_view msg) {
        if (ptr->done) return;
        ptr->done = true;

        if (ptr->on_error)
            ptr->on_error(msg);
        else if (ptr->send_response) {
            HttpResponse resp;
            resp.set_status(502, "Bad Gateway")
                .set_body(fmt::format("upstream error: {}", msg), "text/plain");
            ptr->send_response(resp);
        }
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    };

    ptr->tcp.on_error(error_handler);

    ptr->tcp.on_close([ptr, this] {
        if (ptr->done) return;
        ptr->done = true;
        if (ptr->on_error)
            ptr->on_error("upstream closed connection");
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    });

    ptr->tcp.connect(upstream_host_, upstream_port_);

    contexts_.push_back(std::move(ctx));
}

void HttpProxy::cleanup_done()
{
    contexts_.erase(
        std::remove_if(contexts_.begin(), contexts_.end(),
                       [](const auto& c) { return c->done; }),
        contexts_.end());
}

// ─── HttpProxyManager ────────────────────────────────────────────────────────

HttpProxyManager::HttpProxyManager(EventLoop& loop)
    : loop_(loop)
{}

HttpProxy& HttpProxyManager::add(std::string_view host, uint16_t port)
{
    auto proxy = std::make_unique<HttpProxy>(loop_, host, port);
    auto& ref = *proxy;
    proxies_.push_back(std::move(proxy));
    return ref;
}

void HttpProxyManager::cleanup()
{
    proxies_.clear();
}

} // namespace apostol
