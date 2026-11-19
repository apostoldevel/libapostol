// http_proxy_test — HttpProxy::forward() when it fails, in this process.
//
//   1. an upstream name that does not resolve: the failure is delivered once,
//      from the loop (after forward() has returned), so a handler that
//      destroys the proxy does so with nothing of the proxy on the stack;
//      the proxy is heap-allocated and owned by a unique_ptr, the handler
//      resets it — run under valgrind or a sanitizer this touches no freed
//      memory;
//   2. the same for an upstream that refuses the connection (a closed port
//      of 127.0.0.1), which fails in the loop as it always did;
//   3. without a failure handler, an unresolvable upstream is answered 502
//      through send_response, once, also after forward() returned.
//
// Usage: http_proxy_test

#include "apostol/event_loop.hpp"
#include "apostol/http_proxy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("http_proxy_test: FAIL %s\n", what.c_str());
    }
}

void run_for(EventLoop& loop, std::chrono::milliseconds ms)
{
    loop.add_timer(ms, [&loop] { loop.stop(); }, false);
    loop.run();
}

// A port nothing listens on: bound once, then closed.
std::uint16_t closed_port()
{
    const int   fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len     = sizeof a;
    std::uint16_t port = 0;
    if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0
        && ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    if (fd >= 0)
        ::close(fd);
    return port;
}

struct Outcome
{
    int  failures{0};
    int  responses{0};
    int  status{0};
    bool delivered_before_return{false};
    bool proxy_gone{false};
};

// The proxy is destroyed by its own failure handler.
Outcome fail_and_destroy(const std::string& host, std::uint16_t port)
{
    EventLoop loop;
    Outcome   out;
    auto      proxy = std::make_unique<HttpProxy>(loop, host, port);
    bool      returned = false;

    HttpRequest req;
    req.method = "GET";
    req.path   = "/";
    proxy->forward(
        req, [&out](const HttpResponse&) { ++out.responses; },
        [&](const HttpProxy::ForwardFailure&) {
            ++out.failures;
            if (!returned)
                out.delivered_before_return = true;
            out.proxy_gone = true;
            // Last: the proxy takes this handler's closure with it.
            proxy.reset();
        });
    returned = true;
    // The proxy may be gone already (a failure delivered inside forward()).
    run_for(loop, std::chrono::milliseconds(1500));
    return out;
}

} // namespace

int main()
{
    {
        const auto o = fail_and_destroy("nonexistent.invalid", 80);
        check(o.failures == 1, "unresolvable upstream: failure delivered once, got " + std::to_string(o.failures));
        check(!o.delivered_before_return, "unresolvable upstream: the failure comes after forward() returned");
        check(o.proxy_gone && o.responses == 0, "unresolvable upstream: the handler ran and no response was sent");
    }
    {
        const auto port = closed_port();
        const auto o    = fail_and_destroy("127.0.0.1", port);
        check(o.failures == 1, "refused upstream: failure delivered once, got " + std::to_string(o.failures));
        check(!o.delivered_before_return, "refused upstream: the failure comes after forward() returned");
        check(o.proxy_gone && o.responses == 0, "refused upstream: the handler ran and no response was sent");
    }
    {
        EventLoop   loop;
        HttpProxy   proxy(loop, "nonexistent.invalid", 80);
        int         responses = 0;
        int         status    = 0;
        bool        before    = false;
        bool        returned  = false;
        HttpRequest req;
        req.method = "GET";
        req.path   = "/";
        proxy.forward(req, [&](const HttpResponse& r) {
            ++responses;
            status = r.status_code();
            if (!returned)
                before = true;
        });
        returned = true;
        run_for(loop, std::chrono::milliseconds(1500));
        check(responses == 1 && status == 502, "unresolvable upstream, no failure handler: one 502, got "
                                                   + std::to_string(responses) + " answer(s), status "
                                                   + std::to_string(status));
        check(!before, "unresolvable upstream, no failure handler: the answer comes after forward() returned");
    }

    std::printf("http_proxy_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
