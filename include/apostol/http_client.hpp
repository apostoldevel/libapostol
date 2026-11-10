#pragma once

#include "apostol/event_loop.hpp"
#include "apostol/http.hpp"
#include "apostol/tcp_client.hpp"

#include <chrono>
#include <cstdint>
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
    /// The scheme is http or https (any case; none written — http); any other
    /// is refused with "unsupported URL scheme '<s>'". A URL without a host, with
    /// userinfo, an IPv6 address outside brackets, a port that is not 1..65535 in
    /// digits, or a blank or control byte is refused with "invalid URL". Both are
    /// reported from the loop. The Host header carries the port unless it is the
    /// scheme's default, and an IPv6 address in brackets.
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
        std::string scheme;   // lower case; "http" when none is written
        std::string host;     // empty: no target to connect to; IPv6 without brackets
        uint16_t    port{0};
        std::string path;     // includes query string
    };

    static ParsedUrl parse_url(std::string_view url);
    static std::uint16_t default_port(std::string_view scheme);
    void refuse(ErrorHandler on_error, std::string message);
    void cleanup_done();

    struct PendingRequest;

    EventLoop& loop_;
    std::chrono::milliseconds timeout_{30000};
    std::vector<std::unique_ptr<PendingRequest>> pending_;
    std::vector<EventLoop::TimerId> refusal_timers_;  // refusals in flight: scheme, invalid URL

#ifdef WITH_SSL
    bool tls_enabled_{false};
    bool tls_verify_{true};
#endif
};

} // namespace apostol
