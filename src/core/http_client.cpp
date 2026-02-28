#include "apostol/http_client.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace apostol
{

// ─── PendingRequest ──────────────────────────────────────────────────────────

struct HttpClient::PendingRequest
{
    TcpClient           tcp;
    HttpResponseParser  parser;
    ResponseHandler     on_response;
    ErrorHandler        on_error;
    bool                done{false};

    explicit PendingRequest(EventLoop& loop)
        : tcp(loop) {}
};

// ─── Constructor / Destructor ────────────────────────────────────────────────

HttpClient::HttpClient(EventLoop& loop)
    : loop_(loop)
{}

HttpClient::~HttpClient() = default;

// ─── URL Parsing ─────────────────────────────────────────────────────────────

HttpClient::ParsedUrl HttpClient::parse_url(std::string_view url)
{
    ParsedUrl result;

    // Scheme
    auto scheme_end = url.find("://");
    if (scheme_end != std::string_view::npos) {
        result.scheme = std::string(url.substr(0, scheme_end));
        url.remove_prefix(scheme_end + 3);
    } else {
        result.scheme = "http";
    }

    // Host[:port] / path
    auto slash = url.find('/');
    std::string_view authority = (slash != std::string_view::npos)
        ? url.substr(0, slash)
        : url;

    auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        result.host = std::string(authority.substr(0, colon));
        auto port_sv = authority.substr(colon + 1);
        result.port = 0;
        for (char c : port_sv) {
            if (c >= '0' && c <= '9')
                result.port = result.port * 10 + static_cast<uint16_t>(c - '0');
        }
    } else {
        result.host = std::string(authority);
        result.port = (result.scheme == "https") ? 443 : 80;
    }

    // Path (including query string)
    if (slash != std::string_view::npos)
        result.path = std::string(url.substr(slash));
    else
        result.path = "/";

    return result;
}

// ─── Request Serialization ───────────────────────────────────────────────────

static std::string serialize_request(std::string_view method,
                                      std::string_view host,
                                      std::string_view path,
                                      std::string_view body,
                                      const HttpClient::Headers& headers)
{
    std::string out;
    out.reserve(256 + body.size());

    out += fmt::format("{} {} HTTP/1.1\r\n", method, path);
    out += fmt::format("Host: {}\r\n", host);

    bool has_content_length = false;
    bool has_connection = false;
    for (const auto& [k, v] : headers) {
        out += fmt::format("{}: {}\r\n", k, v);
        // Case-insensitive check
        std::string lower_k = k;
        std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_k == "content-length") has_content_length = true;
        if (lower_k == "connection") has_connection = true;
    }

    if (!has_content_length && !body.empty())
        out += fmt::format("Content-Length: {}\r\n", body.size());

    if (!has_connection)
        out += "Connection: close\r\n";

    out += "\r\n";
    out += body;

    return out;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void HttpClient::request(std::string_view method, std::string_view url_str,
                          std::string_view body, const Headers& headers,
                          ResponseHandler on_response, ErrorHandler on_error)
{
    auto url = parse_url(url_str);

    auto req = std::make_unique<PendingRequest>(loop_);
    auto* ptr = req.get();

    ptr->on_response = std::move(on_response);
    ptr->on_error    = std::move(on_error);

    if (timeout_.count() > 0) {
        ptr->tcp.set_connect_timeout(timeout_);
        ptr->tcp.set_idle_timeout(timeout_);
    }

#ifdef WITH_SSL
    if (tls_enabled_ || url.scheme == "https")
        ptr->tcp.enable_tls(tls_verify_);
#endif

    // Capture serialized request for on_connect
    auto serialized = serialize_request(method, url.host, url.path, body, headers);

    ptr->parser.set_handler([ptr, this](HttpClientResponse resp) {
        if (ptr->done) return;
        ptr->done = true;
        if (ptr->on_response)
            ptr->on_response(std::move(resp));
        ptr->tcp.close();
        // Defer cleanup — we're inside TcpClient's on_data callback chain
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    });

    ptr->tcp.on_connect([ptr, req_data = std::move(serialized)] {
        ptr->tcp.send(req_data);
    });

    ptr->tcp.on_data([ptr](const char* data, size_t len) {
        if (!ptr->done)
            ptr->parser.feed(data, len);
    });

    ptr->tcp.on_error([ptr, this](std::string_view msg) {
        if (ptr->done) return;
        ptr->done = true;
        if (ptr->on_error)
            ptr->on_error(msg);
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    });

    ptr->tcp.on_close([ptr, this] {
        if (ptr->done) return;
        // Connection closed before response completed
        ptr->done = true;
        if (ptr->on_error)
            ptr->on_error("connection closed before response");
        loop_.add_timer(std::chrono::milliseconds(0), [this] { cleanup_done(); }, false);
    });

    ptr->tcp.connect(url.host, url.port);

    pending_.push_back(std::move(req));
}

void HttpClient::get(std::string_view url, const Headers& headers,
                      ResponseHandler on_response, ErrorHandler on_error)
{
    request("GET", url, "", headers, std::move(on_response), std::move(on_error));
}

void HttpClient::post(std::string_view url, std::string_view body,
                       const Headers& headers,
                       ResponseHandler on_response, ErrorHandler on_error)
{
    request("POST", url, body, headers, std::move(on_response), std::move(on_error));
}

void HttpClient::cleanup_done()
{
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(),
                       [](const auto& p) { return p->done; }),
        pending_.end());
}

} // namespace apostol
