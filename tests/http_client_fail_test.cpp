// http_client_fail_test — HttpClient::request() when the connection fails, in
// this process.
//
//   1. a host name that does not resolve (the failure comes from inside
//      TcpClient::connect()): the error is delivered once, from the loop
//      (after request() has returned), so a handler that destroys the client
//      does so with nothing of the client on the stack; the client is
//      heap-allocated and owned by a unique_ptr, the handler resets it — run
//      under valgrind or a sanitizer this touches no freed memory;
//   2. the same for a closed port of 127.0.0.1, which fails in the loop;
//   3. the client kept: an unresolvable host is reported once, after
//      request() returned, no response is delivered, and the client holds no
//      request afterwards (pending_count).
//
// Usage: http_client_fail_test

#include "apostol/event_loop.hpp"
#include "apostol/http_client.hpp"

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
        std::printf("http_client_fail_test: FAIL %s\n", what.c_str());
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
    int  errors{0};
    int  responses{0};
    bool delivered_before_return{false};
    bool client_gone{false};
};

// The client is destroyed by its own error handler.
Outcome fail_and_destroy(const std::string& url)
{
    EventLoop loop;
    Outcome   out;
    auto      client   = std::make_unique<HttpClient>(loop);
    bool      returned = false;

    client->get(
        url, {}, [&out](HttpClientResponse) { ++out.responses; },
        [&](std::string_view) {
            ++out.errors;
            if (!returned)
                out.delivered_before_return = true;
            out.client_gone = true;
            // Last: the client takes this handler's closure with it.
            client.reset();
        });
    returned = true;
    // The client may be gone already (an error delivered inside request()).
    run_for(loop, std::chrono::milliseconds(1500));
    return out;
}

} // namespace

int main()
{
    {
        const auto o = fail_and_destroy("http://nonexistent.invalid/");
        check(o.errors == 1, "unresolvable host: error delivered once, got " + std::to_string(o.errors));
        check(!o.delivered_before_return, "unresolvable host: the error comes after request() returned");
        check(o.client_gone && o.responses == 0, "unresolvable host: the handler ran and no response was delivered");
    }
    {
        const auto port = closed_port();
        const auto o    = fail_and_destroy("http://127.0.0.1:" + std::to_string(port) + "/");
        check(o.errors == 1, "refused port: error delivered once, got " + std::to_string(o.errors));
        check(!o.delivered_before_return, "refused port: the error comes after request() returned");
        check(o.client_gone && o.responses == 0, "refused port: the handler ran and no response was delivered");
    }
    {
        EventLoop  loop;
        HttpClient client(loop);
        int        errors    = 0;
        int        responses = 0;
        bool       before    = false;
        bool       returned  = false;
        std::string message;
        client.get("http://nonexistent.invalid/", {},
                   [&](HttpClientResponse) { ++responses; },
                   [&](std::string_view e) {
                       ++errors;
                       message = std::string(e);
                       if (!returned)
                           before = true;
                   });
        returned = true;
        run_for(loop, std::chrono::milliseconds(1500));
        check(errors == 1 && responses == 0, "unresolvable host, client kept: one error, got "
                                                 + std::to_string(errors) + " error(s), "
                                                 + std::to_string(responses) + " response(s)");
        check(!before, "unresolvable host, client kept: the error comes after request() returned");
        check(message.find("getaddrinfo") != std::string::npos,
              "unresolvable host, client kept: the resolver's message is passed on, got '" + message + "'");
        check(client.pending_count() == 0, "unresolvable host, client kept: no request held, got "
                                               + std::to_string(client.pending_count()));
    }

    std::printf("http_client_fail_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
