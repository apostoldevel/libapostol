// ws_client_test — the port of a WsClient URL, in this process, against a
// listener of its own on 127.0.0.1 (no WebSocket server: only whether the
// client dials the port).
//
//   1. a port of digits 1..65535 is taken as written (leading zeros too), also
//      with a query or a fragment right after it;
//   2. any other port — empty, zero, past 65535, signed, with a blank or a
//      suffix — is reported as "invalid URL" and nothing is dialled; every
//      invalid form is built on the listener's port, so any form that is
//      dialled is seen;
//   3. a URL that does not parse leaves the last good one: reconnect() dials it.
//
// Usage: ws_client_test (exit 77 — skipped — without a loopback listener)

#include "apostol/event_loop.hpp"
#include "apostol/ws_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("ws_client_test: FAIL %s\n", what.c_str());
    }
}

// A non-blocking listener on 127.0.0.1, an ephemeral port.
struct Listener
{
    int           fd{-1};
    std::uint16_t port{0};

    Listener()
    {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len     = sizeof a;
        if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 64) != 0
            || ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return;
        port = ntohs(a.sin_port);
    }
    ~Listener()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    // The connections queued since the last call.
    int drain() const
    {
        int n = 0;
        for (int c; (c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC)) >= 0; ++n)
            ::close(c);
        return n;
    }
};

void run_for(EventLoop& loop, std::chrono::milliseconds ms)
{
    loop.add_timer(ms, [&loop] { loop.stop(); }, false);
    loop.run();
}

// connect(url), then the loop for a moment: the first error reported (empty
// if none), and whether the listener was dialled.
struct Outcome
{
    std::string error;
    bool        dialled{false};
};

Outcome attempt(const Listener& l, const std::string& url)
{
    EventLoop loop;
    Outcome   out;
    {
        WsClient client(loop);
        client.on_error([&out](std::string_view e) {
            if (out.error.empty())
                out.error = std::string(e);
        });
        client.connect(url);
        run_for(loop, std::chrono::milliseconds(100));
    }
    out.dialled = l.drain() > 0;
    return out;
}

std::string shown(const std::string& url, const Outcome& o)
{
    return "'" + url + "': error '" + o.error + "', dialled " + (o.dialled ? "yes" : "no");
}

} // namespace

int main()
{
    const Listener l;
    if (l.port == 0) {
        std::printf("ws_client_test: SKIP no listener on 127.0.0.1\n");
        return 77;
    }
    const std::string p = std::to_string(l.port);
    const std::string h = "ws://127.0.0.1:";

    for (const auto& url : {h + p + "/", h + p, h + p + "/path?q=1", h + "00" + p + "/", h + p + "?token=a:b",
                            h + p + "#frag", h + p + "?q#frag", h + p + "/p#f"}) {
        const auto o = attempt(l, url);
        check(o.error.empty() && o.dialled, shown(url, o));
    }

    // The bounds: taken (dialled elsewhere, refused there — not "invalid URL").
    for (const auto& url : {h + "1/", h + "65535/", h + "00001/"}) {
        const auto o = attempt(l, url);
        check(o.error != "invalid URL" && !o.dialled, shown(url, o));
    }

    const std::vector<std::string> bad = {
        h + "/", h + "0/", h + "000/", h + "65536/", h + "99999999999999999999/", h + "-1/",
        h + std::to_string(l.port + 65536) + "/",
        h + std::to_string(l.port + 4294967296ULL) + "/",
        h + p + "x/", h + p + " /", h + " " + p + "/", h + "+" + p + "/", h + "-" + p + "/",
        h + "0x" + p + "/", h + p + ":" + p + "x/", h + p + "x?q", h + p + "x#f",
        "ws://:" + p + "/",
    };
    for (const auto& url : bad) {
        const auto o = attempt(l, url);
        check(o.error == "invalid URL" && !o.dialled, shown(url, o));
    }

    // A refused URL keeps the last good one; reconnect() dials it.
    {
        EventLoop loop;
        std::vector<std::string> errors;
        WsClient client(loop);
        client.on_error([&errors](std::string_view e) { errors.emplace_back(e); });
        client.connect(h + p + "/");
        run_for(loop, std::chrono::milliseconds(100));
        check(errors.empty() && l.drain() == 1, "first connect");
        client.reconnect();                                 // Reconnecting: connect() is taken
        client.connect(h + p + "x/");
        check(errors.size() == 1 && errors.back() == "invalid URL", "second connect: not 'invalid URL'");
        client.reconnect();
        run_for(loop, std::chrono::milliseconds(1500));   // the first reconnect waits 1 s
        check(l.drain() == 1, "reconnect after a refused URL: the last good URL not dialled");
    }

    // reconnect() before any valid URL: an error, nothing dialled.
    {
        EventLoop loop;
        std::string error;
        WsClient client(loop);
        client.on_error([&error](std::string_view e) { error = std::string(e); });
        client.connect(h + "0/");
        client.reconnect();
        run_for(loop, std::chrono::milliseconds(1500));
        check(error == "reconnect() without a valid URL" && l.drain() == 0, "reconnect without a URL: '" + error + "'");
    }

    std::printf("ws_client_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
