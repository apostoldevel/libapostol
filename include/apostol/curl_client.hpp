#pragma once

#ifdef WITH_CURL

#include "apostol/event_loop.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace apostol
{

// ─── CurlResponse ───────────────────────────────────────────────────────────

struct CurlResponse
{
    int         status_code{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// ─── CurlClient ─────────────────────────────────────────────────────────────
//
// Async libcurl multi wrapper integrated with EventLoop.
// Replaces v1 CCURLClient (delphi/CURL.hpp).
//
// Usage:
//   CurlClient curl(loop);
//   curl.get("https://example.com", {},
//       [](CurlResponse resp) { /* success */ },
//       [](int code, std::string_view err) { /* fail */ });
//
class CurlClient
{
public:
    using DoneHandler  = std::function<void(CurlResponse)>;
    using FailHandler  = std::function<void(int curl_code, std::string_view error)>;
    using WriteHandler = std::function<void(std::string_view chunk)>;

    explicit CurlClient(EventLoop& loop);
    ~CurlClient();

    CurlClient(const CurlClient&) = delete;
    CurlClient& operator=(const CurlClient&) = delete;

    /// Perform an async HTTP request.
    void perform(std::string_view url,
                 std::string_view method,
                 std::string_view content,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 DoneHandler on_done,
                 FailHandler on_fail,
                 WriteHandler on_write = {});

    /// Convenience: async GET.
    void get(std::string_view url,
             const std::vector<std::pair<std::string, std::string>>& headers,
             DoneHandler on_done,
             FailHandler on_fail);

    /// Convenience: async POST.
    void post(std::string_view url,
              std::string_view content,
              const std::vector<std::pair<std::string, std::string>>& headers,
              DoneHandler on_done,
              FailHandler on_fail);

    void set_timeout(long ms) noexcept { timeout_ms_ = ms; }
    void set_proxy(std::string_view proxy) { proxy_ = std::string(proxy); }

private:
    struct Transfer;

    // curl_multi callbacks (static)
    static int socket_callback(CURL* easy, curl_socket_t s, int what,
                               void* clientp, void* socketp);
    static int timer_callback(CURLM* multi, long timeout_ms, void* clientp);

    // curl_easy callbacks (static)
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata);

    void check_multi_info();

    EventLoop&  loop_;
    CURLM*      multi_{nullptr};
    EventLoop::TimerId timer_id_{EventLoop::kInvalidTimer};
    long        timeout_ms_{0};
    std::string proxy_;

    // Track registered socket fds so we can clean up properly
    std::vector<curl_socket_t> registered_fds_;
};

} // namespace apostol

#endif // WITH_CURL
