// fetch_limit_test — the limit on a response of HttpClient, CurlClient and
// FetchClient (both backends), in this process, against servers of its own on
// 127.0.0.1 that send a response without end.
//
// For each client, with a limit of 1 MiB:
//   1. a body announced by Content-Length far over the limit: the request
//      fails with "response larger than 1048576 bytes", and the server's
//      sending stops soon after (the connection is closed);
//   2. a chunked body without end, and a body without length, until close:
//      the same failure;
//   3. a response of 1 KiB, under the limit: delivered, status 200.
//
// Usage: fetch_limit_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/fetch_client.hpp"
#include "apostol/http_client.hpp"
#ifdef WITH_CURL
#include "apostol/curl_client.hpp"
#endif

#include <signal.h>

#include <atomic>
#include <string>

using namespace apostol;
using namespace nettest;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("fetch_limit_test: FAIL %s\n", what.c_str());
    }
}

constexpr std::size_t kLimit    = 1u << 20;
constexpr std::size_t kSendCap  = 96u << 20;   // the server stops here on its own
constexpr std::size_t kSendFail = 48u << 20;   // more than this sent: the limit did not hold

enum class Body { announced, chunked, until_close, small };

// A server whose answer is `body` kind; `sent` counts the body bytes it got
// onto the wire.
class Endless
{
public:
    explicit Endless(Body kind)
        : server_([this, kind](Conn& c, int) {
              std::string req;
              if (!read_until(c, req, "\r\n\r\n"))
                  return;
              if (kind == Body::small) {
                  c.write("HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nConnection: close\r\n\r\n" + std::string(1024, 'x'));
                  c.close_now();
                  return;
              }
              std::string head = "HTTP/1.1 200 OK\r\n";
              if (kind == Body::announced)
                  head += "Content-Length: 10737418240\r\nConnection: close\r\n\r\n";
              else if (kind == Body::chunked)
                  head += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
              else
                  head += "Connection: close\r\n\r\n";
              if (!c.write(head))
                  return;
              const std::string block(65536, 'z');
              const std::string chunk = "10000\r\n" + block + "\r\n";
              while (sent_ < kSendCap && !c.stopping()) {
                  if (!c.write(kind == Body::chunked ? chunk : block, 1000))
                      break;
                  sent_ += block.size();
              }
              c.close_now();
          })
    {}

    bool          ok() const { return server_.ok(); }
    std::string   url() const { return "http://127.0.0.1:" + std::to_string(server_.port()) + "/"; }
    std::size_t   sent() const { return sent_.load(); }

private:
    std::atomic<std::size_t> sent_{0};
    Server                   server_;
};

struct Result
{
    bool        responded{false};
    int         status{0};
    std::size_t body_size{0};
    bool        failed{false};
    std::string error;
    bool        finished() const { return responded || failed; }
};

const char* const kLimitText = "response larger than 1048576 bytes";

// The limit is set on every client through this one call each.
template <class Client>
void apply_limit(Client& c)
{
    c.set_max_response_size(kLimit);
}

void judge(const std::string& who, const std::string& what, Body kind, const Result& r, const Endless& srv)
{
    const std::string label = who + ", " + what;
    if (kind == Body::small) {
        check(r.responded && r.status == 200 && r.body_size == 1024 && !r.failed,
              label + ": 1 KiB under the limit delivered, status " + std::to_string(r.status) + ", body "
                  + std::to_string(r.body_size) + ", error '" + r.error + "'");
        return;
    }
    check(r.failed && !r.responded && r.error == kLimitText,
          label + ": failed with the limit text, got '" + r.error + "' (responded " + (r.responded ? "yes" : "no") + ")");
    check(srv.sent() < kSendFail, label + ": the server sent " + std::to_string(srv.sent() >> 20)
                                      + " MiB (limit not enforced if over 48)");
}

void with_http(Body kind, const std::string& what)
{
    Endless   srv(kind);
    EventLoop loop;
    Result    r;
    HttpClient c(loop);
    apply_limit(c);
    c.get(srv.url(), {},
          [&](HttpClientResponse resp) { r.responded = true; r.status = resp.status_code; r.body_size = resp.body.size(); },
          [&](std::string_view e) { r.failed = true; r.error = std::string(e); });
    run_until(loop, [&] { return r.finished(); }, 6000);
    settle(loop, 100);
    judge("HttpClient", what, kind, r, srv);
}

#ifdef WITH_CURL
void with_curl(Body kind, const std::string& what)
{
    Endless    srv(kind);
    EventLoop  loop;
    Result     r;
    CurlClient c(loop);
    apply_limit(c);
    c.get(srv.url(), {},
          [&](CurlResponse resp) { r.responded = true; r.status = resp.status_code; r.body_size = resp.body.size(); },
          [&](int, std::string_view e) { r.failed = true; r.error = std::string(e); });
    run_until(loop, [&] { return r.finished(); }, 6000);
    settle(loop, 100);
    judge("CurlClient", what, kind, r, srv);
}
#endif

void with_fetch(FetchClient::Backend backend, const std::string& name, Body kind, const std::string& what)
{
    Endless     srv(kind);
    EventLoop   loop;
    Result      r;
    FetchClient c(loop, backend);
    apply_limit(c);
    c.get(srv.url(), {},
          [&](FetchResponse resp) { r.responded = true; r.status = resp.status_code; r.body_size = resp.body.size(); },
          [&](std::string_view e) { r.failed = true; r.error = std::string(e); });
    run_until(loop, [&] { return r.finished(); }, 6000);
    settle(loop, 100);
    judge(name, what, kind, r, srv);
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    {
        Endless probe(Body::small);
        if (!probe.ok()) {
            std::printf("fetch_limit_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    const std::pair<Body, const char*> kinds[] = {
        {Body::announced, "announced length over the limit"},
        {Body::chunked, "chunked body without end"},
        {Body::until_close, "body without length"},
        {Body::small, "small response"},
    };
    for (const auto& [kind, what] : kinds) {
        with_http(kind, what);
#ifdef WITH_CURL
        with_curl(kind, what);
        with_fetch(FetchClient::Backend::curl, "FetchClient(curl)", kind, what);
#endif
        with_fetch(FetchClient::Backend::http, "FetchClient(http)", kind, what);
    }

    std::printf("fetch_limit_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
