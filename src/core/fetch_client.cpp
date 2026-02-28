#include "apostol/fetch_client.hpp"

#include <chrono>

namespace apostol
{

FetchClient::FetchClient(EventLoop& loop, Backend backend)
{
#ifdef WITH_CURL
    if (backend == Backend::curl || backend == Backend::auto_select)
        curl_ = std::make_unique<CurlClient>(loop);
    else
        http_ = std::make_unique<HttpClient>(loop);
#else
    (void)backend;
    http_ = std::make_unique<HttpClient>(loop);
#endif
}

FetchClient::~FetchClient() = default;

FetchClient::Backend FetchClient::active_backend() const
{
#ifdef WITH_CURL
    if (curl_) return Backend::curl;
#endif
    return Backend::http;
}

// ─── request ───────────────────────────────────────────────────────────────

void FetchClient::request(std::string_view method, std::string_view url,
                          std::string_view body, const Headers& headers,
                          DoneHandler on_done, ErrorHandler on_error)
{
#ifdef WITH_CURL
    if (curl_) {
        curl_->perform(url, method, body, headers,
            [on_done = std::move(on_done)](CurlResponse r) {
                on_done({r.status_code, std::move(r.body), std::move(r.headers)});
            },
            [on_error = std::move(on_error)](int /*code*/, std::string_view err) {
                on_error(err);
            });
        return;
    }
#endif
    http_->request(method, url, body, headers,
        [on_done = std::move(on_done)](HttpClientResponse r) {
            on_done({r.status_code, std::move(r.body), std::move(r.headers)});
        },
        std::move(on_error));
}

// ─── get ───────────────────────────────────────────────────────────────────

void FetchClient::get(std::string_view url, const Headers& headers,
                      DoneHandler on_done, ErrorHandler on_error)
{
#ifdef WITH_CURL
    if (curl_) {
        curl_->get(url, headers,
            [on_done = std::move(on_done)](CurlResponse r) {
                on_done({r.status_code, std::move(r.body), std::move(r.headers)});
            },
            [on_error = std::move(on_error)](int /*code*/, std::string_view err) {
                on_error(err);
            });
        return;
    }
#endif
    http_->get(url, headers,
        [on_done = std::move(on_done)](HttpClientResponse r) {
            on_done({r.status_code, std::move(r.body), std::move(r.headers)});
        },
        std::move(on_error));
}

// ─── post ──────────────────────────────────────────────────────────────────

void FetchClient::post(std::string_view url, std::string_view body,
                       const Headers& headers,
                       DoneHandler on_done, ErrorHandler on_error)
{
#ifdef WITH_CURL
    if (curl_) {
        curl_->post(url, body, headers,
            [on_done = std::move(on_done)](CurlResponse r) {
                on_done({r.status_code, std::move(r.body), std::move(r.headers)});
            },
            [on_error = std::move(on_error)](int /*code*/, std::string_view err) {
                on_error(err);
            });
        return;
    }
#endif
    http_->post(url, body, headers,
        [on_done = std::move(on_done)](HttpClientResponse r) {
            on_done({r.status_code, std::move(r.body), std::move(r.headers)});
        },
        std::move(on_error));
}

// ─── set_timeout ───────────────────────────────────────────────────────────

void FetchClient::set_timeout(long ms)
{
#ifdef WITH_CURL
    if (curl_) {
        curl_->set_timeout(ms);
        return;
    }
#endif
    http_->set_timeout(std::chrono::milliseconds(ms));
}

// ─── set_proxy (curl only) ─────────────────────────────────────────────────

#ifdef WITH_CURL
void FetchClient::set_proxy(std::string_view proxy)
{
    if (curl_)
        curl_->set_proxy(proxy);
}
#endif

} // namespace apostol
