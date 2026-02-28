#pragma once

#include "apostol/event_loop.hpp"
#include "apostol/http.hpp"
#include "apostol/tcp_client.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── HttpClient ─────────────────────────────────────────────────────────────
//
// Native async HTTP client built on TcpClient + HttpResponseParser.
// Complements CurlClient (not replaces). For internal calls and proxy relay.
//
// Usage:
//   HttpClient http(loop);
//   http.get("http://127.0.0.1:8080/api/status", {},
//       [](HttpClientResponse r) { fmt::print("{}\n", r.body); },
//       [](std::string_view e) { fmt::print(stderr, "{}\n", e); });
//
class HttpClient
{
public:
    using ResponseHandler = std::function<void(HttpClientResponse)>;
    using ErrorHandler    = std::function<void(std::string_view)>;
    using Headers         = std::vector<std::pair<std::string, std::string>>;

    explicit HttpClient(EventLoop& loop);
    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&)                 = delete;
    HttpClient& operator=(HttpClient&&)      = delete;

    /// Full request with method, url, body, headers.
    void request(std::string_view method, std::string_view url,
                 std::string_view body, const Headers& headers,
                 ResponseHandler on_response, ErrorHandler on_error);

    /// GET shorthand.
    void get(std::string_view url, const Headers& headers,
             ResponseHandler on_response, ErrorHandler on_error);

    /// POST shorthand.
    void post(std::string_view url, std::string_view body,
              const Headers& headers,
              ResponseHandler on_response, ErrorHandler on_error);

    void set_timeout(std::chrono::milliseconds ms) { timeout_ = ms; }

#ifdef WITH_SSL
    void set_tls(bool enable = true, bool verify = true)
    {
        tls_enabled_ = enable;
        tls_verify_  = verify;
    }
#endif

private:
    struct ParsedUrl {
        std::string scheme;   // "http" or "https"
        std::string host;
        uint16_t    port{0};
        std::string path;     // includes query string
    };

    static ParsedUrl parse_url(std::string_view url);
    void cleanup_done();

    struct PendingRequest;

    EventLoop& loop_;
    std::chrono::milliseconds timeout_{30000};
    std::vector<std::unique_ptr<PendingRequest>> pending_;

#ifdef WITH_SSL
    bool tls_enabled_{false};
    bool tls_verify_{true};
#endif
};

} // namespace apostol
