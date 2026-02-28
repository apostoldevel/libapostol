#pragma once

#ifdef WITH_CURL
#include "apostol/curl_client.hpp"
#endif
#include "apostol/http_client.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── FetchResponse ─────────────────────────────────────────────────────────
// Unified response: subset of both CurlResponse and HttpClientResponse.

struct FetchResponse
{
    int         status_code{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// ─── FetchClient ───────────────────────────────────────────────────────────
//
// Runtime-selectable wrapper: CurlClient or HttpClient.
// Default (auto_select): CurlClient when WITH_CURL, HttpClient otherwise.
// Modules use FetchClient — no #ifdef WITH_CURL in module code.
//
// Usage:
//   FetchClient fetch(loop);                               // auto
//   FetchClient fetch(loop, FetchClient::Backend::http);   // force HttpClient
//   FetchClient fetch(loop, FetchClient::Backend::curl);   // force CurlClient
//
class FetchClient
{
public:
    using Headers      = std::vector<std::pair<std::string, std::string>>;
    using DoneHandler  = std::function<void(FetchResponse)>;
    using ErrorHandler = std::function<void(std::string_view)>;

    enum class Backend { auto_select, curl, http };

    explicit FetchClient(EventLoop& loop, Backend backend = Backend::auto_select);
    ~FetchClient();

    FetchClient(const FetchClient&) = delete;
    FetchClient& operator=(const FetchClient&) = delete;

    /// Which backend is actually active.
    Backend active_backend() const;

    void request(std::string_view method, std::string_view url,
                 std::string_view body, const Headers& headers,
                 DoneHandler on_done, ErrorHandler on_error);

    void get(std::string_view url, const Headers& headers,
             DoneHandler on_done, ErrorHandler on_error);

    void post(std::string_view url, std::string_view body,
              const Headers& headers,
              DoneHandler on_done, ErrorHandler on_error);

    void set_timeout(long ms);

#ifdef WITH_CURL
    void set_proxy(std::string_view proxy);
#endif

private:
#ifdef WITH_CURL
    std::unique_ptr<CurlClient> curl_;
#endif
    std::unique_ptr<HttpClient> http_;
};

} // namespace apostol
