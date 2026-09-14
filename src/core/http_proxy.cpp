#include "apostol/http_proxy.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace apostol
{

// ─── ForwardCtx ──────────────────────────────────────────────────────────────

struct HttpProxy::ForwardCtx
{
    EventLoop&          loop;
    TcpClient           tcp;
    HttpResponseParser  parser;
    SendResponse        send_response;
    OnFailure           on_failure;
    bool                done{false};
    bool                request_sent{false};
    EventLoop::TimerId  response_timer{EventLoop::kInvalidTimer};

    explicit ForwardCtx(EventLoop& l) : loop(l), tcp(l) {}

    ~ForwardCtx() { cancel_response_timer(); }

    void cancel_response_timer()
    {
        if (response_timer != EventLoop::kInvalidTimer) {
            loop.cancel_timer(response_timer);
            response_timer = EventLoop::kInvalidTimer;
        }
    }
};

// ─── HttpProxy ───────────────────────────────────────────────────────────────

HttpProxy::HttpProxy(EventLoop& loop, std::string_view upstream_host, uint16_t upstream_port)
    : loop_(loop)
    , upstream_host_(upstream_host)
    , upstream_port_(upstream_port)
{}

HttpProxy::~HttpProxy()
{
    if (cleanup_timer_ != EventLoop::kInvalidTimer)
        loop_.cancel_timer(cleanup_timer_);
}

void HttpProxy::forward(const HttpRequest& req, SendResponse send_response,
                         std::function<void(std::string_view)> on_error)
{
    OnFailure on_failure;
    if (on_error)
        on_failure = [on_error = std::move(on_error)](const ForwardFailure& f) {
            on_error(f.message);
        };
    forward_impl(req, std::move(send_response), std::move(on_failure));
}

void HttpProxy::forward(const HttpRequest& req, SendResponse send_response, OnFailure on_failure)
{
    forward_impl(req, std::move(send_response), std::move(on_failure));
}

void HttpProxy::forward_impl(const HttpRequest& req, SendResponse send_response,
                              OnFailure on_failure)
{
    using Reason = ForwardFailure::Reason;

    auto ctx = std::make_unique<ForwardCtx>(loop_);
    auto* ptr = ctx.get();

    ptr->send_response = std::move(send_response);
    ptr->on_failure    = std::move(on_failure);

    // The idle timer must not cut a response deadline short: a silent upstream
    // is reported at whichever of the two is earlier.
    const auto idle_timeout = std::max(timeout_, response_timeout_);
    if (idle_timeout.count() > 0)
        ptr->tcp.set_idle_timeout(idle_timeout);

    const auto connect_timeout = connect_timeout_.count() > 0 ? connect_timeout_ : timeout_;
    if (connect_timeout.count() > 0)
        ptr->tcp.set_connect_timeout(connect_timeout);

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

    // Add X-Forwarded-For — appended to whatever the request already carries,
    // so a caller behind an outer proxy turns this off and keeps the one header
    // that names the client (set_append_forwarded_for).
    if (append_forwarded_for_ && !req.peer_ip.empty())
        serialized += fmt::format("X-Forwarded-For: {}\r\n", req.peer_ip);

    serialized += "Connection: close\r\n\r\n";
    serialized += req.body;

    // Reporting a failure is the LAST thing done with ptr and this: the
    // caller's callback may destroy the proxy (a gateway dropping an upstream
    // it just found dead), and the sweep is scheduled before it so that the
    // destructor can cancel it. The report itself does not check `done` —
    // the callers below do, each in its own way.
    auto report = [ptr, this](Reason reason, std::string_view msg) {
        ptr->cancel_response_timer();
        schedule_cleanup();

        if (ptr->on_failure)
            ptr->on_failure(ForwardFailure{reason, msg, ptr->request_sent});
        else if (ptr->send_response) {
            HttpResponse resp;
            resp.set_status(502, "Bad Gateway")
                .set_body(fmt::format("upstream error: {}", msg), "text/plain");
            ptr->send_response(resp);
        }
    };

    // Every way out of a forward that is not an upstream response. Runs once:
    // the tcp callbacks and the response timer can each fire after another
    // has already settled the context.
    auto fail = [ptr, report](Reason reason, std::string_view msg) {
        if (ptr->done) return;
        ptr->done = true;
        report(reason, msg);
    };

    // Set up response parsing
    ptr->parser.set_handler([ptr, this](HttpClientResponse upstream_resp) {
        if (ptr->done) return;
        ptr->done = true;
        ptr->cancel_response_timer();

        // Build server-side HttpResponse from upstream response
        HttpResponse resp;
        resp.set_status(upstream_resp.status_code, upstream_resp.status_text);

        // Content-Length is serialize()'s: it always writes its own from the
        // body, so a copied one went out twice. Content-Type goes through
        // set_body(), whose default would otherwise overwrite the copied one
        // with text/plain — and set_header() matches the name case-sensitively,
        // so a lowercase "content-type" from the upstream would have survived
        // next to it. Both taken out of the loop, the type handed to set_body.
        std::string content_type;
        bool has_content_type = false;
        for (const auto& [k, v] : upstream_resp.headers) {
            // Skip hop-by-hop headers
            std::string lower_k = k;
            std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower_k == "transfer-encoding" || lower_k == "connection"
                || lower_k == "content-length")
                continue;
            if (lower_k == "content-type") {
                content_type = v;
                has_content_type = true;
                continue;
            }
            resp.add_header(k, v);
        }

        if (!upstream_resp.body.empty()) {
            resp.set_body(std::move(upstream_resp.body), content_type);
            if (!has_content_type)
                resp.del_header("Content-Type");   // relay what came, invent nothing
        } else if (has_content_type) {
            resp.set_header("Content-Type", content_type);
        }

        // Close and schedule the sweep before handing the response over: the
        // recipient may destroy this proxy, and we are inside TcpClient's
        // on_data chain (close() finds the context done and reports nothing).
        ptr->tcp.close();
        schedule_cleanup();

        if (ptr->send_response)
            ptr->send_response(resp);
    });

    ptr->tcp.on_connect([ptr, report, data = std::move(serialized),
                         response_timeout = response_timeout_] {
        // From here on the upstream may act on the request: a failure is no
        // longer safe to retry elsewhere, whatever comes back on the socket.
        ptr->request_sent = true;
        ptr->tcp.send(data);

        // The idle timer restarts on every byte; this one does not.
        if (response_timeout.count() > 0)
            ptr->response_timer = ptr->loop.add_timer(response_timeout, [ptr, report] {
                ptr->response_timer = EventLoop::kInvalidTimer;
                if (ptr->done) return;
                ptr->done = true;
                // Mark done, then close — close() reports on_close, which must
                // find the context settled — and only then tell the caller.
                ptr->tcp.close();
                report(Reason::response_timeout, "response timeout");
            }, /*repeat=*/false);
    });

    ptr->tcp.on_data([ptr](const char* data, size_t len) {
        if (!ptr->done)
            ptr->parser.feed(data, len);
    });

    ptr->tcp.on_error([ptr, fail](std::string_view msg) {
        // Before the request went out every error is a connect failure,
        // whatever TcpClient calls it (refused, unreachable, its connect
        // timer, TLS). After it, its idle timer is the one text worth telling
        // apart — the message is TcpClient's own, one place to keep in step.
        const auto reason = !ptr->request_sent ? Reason::connect
                          : msg == "idle timeout" ? Reason::idle_timeout
                          : Reason::other;
        fail(reason, msg);
    });

    ptr->tcp.on_close([ptr, fail] {
        fail(ptr->request_sent ? Reason::closed : Reason::connect,
             "upstream closed connection");
    });

    ptr->tcp.connect(upstream_host_, upstream_port_);

    contexts_.push_back(std::move(ctx));
}

void HttpProxy::schedule_cleanup()
{
    if (cleanup_timer_ != EventLoop::kInvalidTimer)
        return;   // one sweep pending already covers every settled context
    cleanup_timer_ = loop_.add_timer(std::chrono::milliseconds(0), [this] {
        cleanup_timer_ = EventLoop::kInvalidTimer;
        cleanup_done();
    }, /*repeat=*/false);
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
