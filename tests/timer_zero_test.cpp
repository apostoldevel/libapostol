// timer_zero_test — a timer of zero interval, and the clients whose clean-up
// rides on one, in this process, against a server of its own on 127.0.0.1.
//
//   1. EventLoop::add_timer(0) runs its callback once, on the next turn of the
//      loop (not from inside add_timer), and releases its timerfd; the same
//      with repeat=true (it fires once, not in a spin) and with a negative
//      interval;
//   2. HttpClient, 10 and 100 requests answered 200, 10 over the response
//      limit, 10 cut short by the server, 10 refused: every request has one
//      outcome (counted once more after a settle); afterwards the client holds no request
//      (pending_count) and the process has no more timerfds than before;
//   3. HttpClient destroyed after its requests settled and before the sweep
//      of finished requests ran: the sweep then runs on the loop and touches
//      nothing of the client (run under valgrind or a sanitizer);
//   4. HttpProxy, 10 and 100 forwards answered 200: every response delivered,
//      afterwards no context held (context_count) and no timerfd left; the
//      proxy destroyed before its sweep ran, as in 3.
//
// Usage: timer_zero_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/http.hpp"
#include "apostol/http_client.hpp"
#include "apostol/http_proxy.hpp"

#include <dirent.h>

#include <cstring>
#include <memory>
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
        std::printf("timer_zero_test: FAIL %s\n", what.c_str());
    }
}

// Open descriptors of this process that are timerfds.
int timerfds()
{
    int  n   = 0;
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir)
        return -1;
    while (auto* e = ::readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        char    link[256];
        const std::string path = std::string("/proc/self/fd/") + e->d_name;
        const auto len = ::readlink(path.c_str(), link, sizeof link - 1);
        if (len <= 0)
            continue;
        link[len] = '\0';
        if (std::strstr(link, "timerfd"))
            ++n;
    }
    ::closedir(dir);
    return n;
}

// A server that answers every request 200 "ok" and closes — or, cut, sends
// the head and half the body and closes.
class Ok
{
public:
    explicit Ok(bool cut = false)
        : server_([cut](Conn& c, int) {
              std::string req;
              if (!read_until(c, req, "\r\n\r\n"))
                  return;
              c.write(cut ? "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nok"
                          : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
          })
    {}
    bool          ok() const { return server_.ok(); }
    std::uint16_t port() const { return server_.port(); }
    std::string   url() const { return "http://127.0.0.1:" + std::to_string(port()) + "/"; }

private:
    Server server_;
};

// ─── 1. the loop ─────────────────────────────────────────────────────────────

void zero_timer(const char* what, std::chrono::milliseconds interval, bool repeat)
{
    EventLoop  loop;
    const int  before = timerfds();
    int        fired  = 0;
    loop.add_timer(interval, [&fired] { ++fired; }, repeat);
    check(fired == 0, std::string(what) + ": not called from inside add_timer");

    // The callback is due on the next turn: well before this stop.
    bool fired_before_stop = false;
    loop.add_timer(200ms, [&] { fired_before_stop = (fired == 1); loop.stop(); }, false);
    loop.run();

    check(fired == 1, std::string(what) + ": fired once (fired " + std::to_string(fired) + ")");
    check(fired_before_stop, std::string(what) + ": fired before a 200 ms timer");
    check(timerfds() == before, std::string(what) + ": timerfd released (" + std::to_string(timerfds())
                                    + " open, " + std::to_string(before) + " before)");
}

// ─── 2, 3. HttpClient ────────────────────────────────────────────────────────

// expect_ok: every request answered 200; otherwise every one fails (a limit
// under the response, a response cut short).
void client_requests(const Ok& srv, int n, const std::string& name = "client",
                     bool expect_ok = true, std::size_t limit = 0)
{
    EventLoop  loop;
    const int  before = timerfds();
    HttpClient http(loop);
    int        ok = 0, failed = 0;
    if (limit)
        http.set_max_response_size(limit);

    for (int i = 0; i < n; ++i)
        http.get(srv.url(), {},
                 [&ok](HttpClientResponse r) { if (r.status_code == 200) ++ok; },
                 [&failed](std::string_view) { ++failed; });

    run_until(loop, [&] { return ok + failed == n; }, 10000);
    settle(loop, 50);

    const std::string tag = name + " " + std::to_string(n) + ": ";
    check(expect_ok ? (ok == n && failed == 0) : (ok == 0 && failed == n), tag + std::to_string(ok) + " answered 200, " + std::to_string(failed) + " failed");
    check(http.pending_count() == 0, tag + std::to_string(http.pending_count()) + " requests still held");
    check(timerfds() == before, tag + std::to_string(timerfds() - before) + " timerfds left");
}

void client_refused(int n)
{
    EventLoop  loop;
    ClosedPort closed;
    const int  before = timerfds();
    HttpClient http(loop);
    int        failed = 0, ok = 0;
    const auto url = "http://127.0.0.1:" + std::to_string(closed.port()) + "/";

    for (int i = 0; i < n; ++i)
        http.get(url, {}, [&ok](HttpClientResponse) { ++ok; }, [&failed](std::string_view) { ++failed; });

    run_until(loop, [&] { return failed + ok == n; }, 10000);
    settle(loop, 50);

    check(failed == n && ok == 0, "refused: " + std::to_string(failed) + " of " + std::to_string(n) + " failed");
    check(http.pending_count() == 0, "refused: " + std::to_string(http.pending_count()) + " requests still held");
    check(timerfds() == before, "refused: " + std::to_string(timerfds() - before) + " timerfds left");
}

void client_gone_before_sweep(const Ok& srv)
{
    EventLoop  loop;
    const int  before = timerfds();
    auto       http   = std::make_unique<HttpClient>(loop);
    int        ok     = 0;

    // The loop stops from inside the handler: the sweep scheduled by the
    // settled request is still pending when run() returns.
    http->get(srv.url(), {},
              [&](HttpClientResponse) { ++ok; loop.stop(); },
              [&](std::string_view) { loop.stop(); });
    const auto guard = loop.add_timer(5000ms, [&loop] { loop.stop(); }, false);
    loop.run();
    loop.cancel_timer(guard);
    check(ok == 1, "client gone: answered");
    check(http->pending_count() == 1, "client gone: the sweep had not run yet ("
                                          + std::to_string(http->pending_count()) + " held)");

    http.reset();
    settle(loop, 50);   // the sweep fires, finds the client gone
    check(timerfds() == before, "client gone: " + std::to_string(timerfds() - before) + " timerfds left");
}

// ─── 4. HttpProxy ────────────────────────────────────────────────────────────

HttpRequest get_request()
{
    HttpRequest req;
    req.method  = "GET";
    req.path    = "/";
    req.version = "HTTP/1.1";
    req.peer_ip = "127.0.0.1";
    return req;
}

void proxy_forwards(const Ok& srv, int n)
{
    EventLoop loop;
    const int before = timerfds();
    HttpProxy proxy(loop, "127.0.0.1", srv.port());
    int       ok = 0, other = 0;

    for (int i = 0; i < n; ++i)
        proxy.forward(get_request(),
                      [&](const HttpResponse& r) { if (r.status_code() == 200) ++ok; else ++other; },
                      std::function<void(std::string_view)>([&other](std::string_view) { ++other; }));

    run_until(loop, [&] { return ok + other == n; }, 10000);
    settle(loop, 50);

    const std::string tag = "proxy " + std::to_string(n) + ": ";
    check(ok == n && other == 0, tag + std::to_string(ok) + " answered 200, " + std::to_string(other) + " not");
    check(proxy.context_count() == 0, tag + std::to_string(proxy.context_count()) + " contexts still held");
    check(timerfds() == before, tag + std::to_string(timerfds() - before) + " timerfds left");
}

void proxy_gone_before_sweep(const Ok& srv)
{
    EventLoop loop;
    const int before = timerfds();
    auto      proxy  = std::make_unique<HttpProxy>(loop, "127.0.0.1", srv.port());
    int       ok     = 0;

    proxy->forward(get_request(),
                   [&](const HttpResponse&) { ++ok; loop.stop(); },
                   std::function<void(std::string_view)>([&](std::string_view) { loop.stop(); }));
    const auto guard = loop.add_timer(5000ms, [&loop] { loop.stop(); }, false);
    loop.run();
    loop.cancel_timer(guard);
    check(ok == 1, "proxy gone: answered");
    check(proxy->context_count() == 1, "proxy gone: the sweep had not run yet ("
                                           + std::to_string(proxy->context_count()) + " held)");

    proxy.reset();
    settle(loop, 50);
    check(timerfds() == before, "proxy gone: " + std::to_string(timerfds() - before) + " timerfds left");
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);

    zero_timer("zero, one-shot", 0ms, false);
    zero_timer("zero, repeat", 0ms, true);
    zero_timer("negative", -5ms, false);

    Ok srv;
    if (!srv.ok()) {
        std::printf("timer_zero_test: no loopback listener — skipped\n");
        return 77;
    }

    client_requests(srv, 10);
    client_requests(srv, 100);
    client_requests(srv, 10, "over limit", false, 16);
    {
        Ok cut(true);
        client_requests(cut, 10, "cut short", false);
    }
    client_refused(10);
    client_gone_before_sweep(srv);

    proxy_forwards(srv, 10);
    proxy_forwards(srv, 100);
    proxy_gone_before_sweep(srv);

    if (g_failed) {
        std::printf("timer_zero_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("timer_zero_test: OK\n");
    return 0;
}
