// ws_close_test — how WsClient ends a connection, in this process, against
// servers of its own on 127.0.0.1.
//
//   1. a refused opening handshake (a 403 answer; a 101 with a wrong
//      Sec-WebSocket-Accept) on a socket the server keeps open: on_error
//      reports it, the connection is dropped, and the next connect() dials a
//      second connection instead of "connect() called in invalid state";
//   2. close() with a server that ignores the Close frame: on_close(1006,
//      "close timeout") once, after the close timeout;
//   3. close() with a server that drops the connection on the Close frame:
//      on_close(1006, ...) once;
//   4. close() with a server that answers with its own Close frame: on_close
//      with the server's code once, and no second call when the close timeout
//      would have elapsed.
//
// Usage: ws_close_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/ws_client.hpp"

#include <signal.h>

#include <string>
#include <vector>

using namespace apostol;
using namespace nettest;

#ifndef WITH_SSL

int main()
{
    std::printf("ws_close_test: SKIP built without WITH_SSL\n");
    return 77;
}

#else

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("ws_close_test: FAIL %s\n", what.c_str());
    }
}

std::string url_of(const Server& s)
{
    return "ws://127.0.0.1:" + std::to_string(s.port()) + "/";
}

// Answers the upgrade as asked, then holds the socket open until the client
// goes.
void test_refused_upgrade(const std::string& what, const std::string& answer, bool wrong_accept,
                          const std::string& expect)
{
    Server srv([&](Conn& c, int) {
        if (wrong_accept) {
            ws_handshake(c, false);
        } else {
            std::string req;
            if (!read_until(c, req, "\r\n\r\n"))
                return;
            c.write(answer);
        }
        std::string b;
        while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
    });

    EventLoop                loop;
    WsClient                 client(loop);
    std::vector<std::string> errors;
    client.auto_reconnect(false);
    client.on_error([&](std::string_view e) { errors.emplace_back(e); });

    client.connect(url_of(srv));
    run_until(loop, [&] { return !errors.empty(); }, 3000);
    check(errors.size() == 1 && errors[0].rfind(expect, 0) == 0,
          what + ": first error '" + (errors.empty() ? "" : errors[0]) + "', expected '" + expect + "...'");

    client.connect(url_of(srv));
    run_until(loop, [&] { return srv.accepted() >= 2 && errors.size() >= 2; }, 3000);
    check(srv.accepted() == 2, what + ": the second connect() dialled a second connection, connections "
                                   + std::to_string(srv.accepted()));
    check(errors.size() == 2 && errors[1].rfind(expect, 0) == 0,
          what + ": second error '" + (errors.size() < 2 ? "" : errors[1]) + "', expected '" + expect + "...'");
}

struct CloseResult
{
    int         calls{0};
    int         code{0};
    std::string reason;
    long        at_ms{0};
};

enum class Peer { ignore_close, drop_on_close, echo_close };

// Connect, call close() once connected, and watch on_close for `wait_ms`.
CloseResult close_with(Peer peer, int wait_ms)
{
    Server srv([peer](Conn& c, int) {
        if (!ws_handshake(c))
            return;
        std::string b;
        // The client's Close frame: opcode 0x8 in the first byte.
        while (!c.stopping() && c.read(b, 200) != Rd::eof) {
            if (b.empty() || (static_cast<unsigned char>(b[0]) & 0x0F) != 0x8)
                continue;
            if (peer == Peer::drop_on_close) {
                c.close_now();
                return;
            }
            if (peer == Peer::echo_close)
                c.write(std::string("\x88\x02\x03\xE8", 4));   // Close, 1000
            b.clear();
        }
    });

    EventLoop   loop;
    WsClient    client(loop);
    CloseResult r;
    const auto  t0 = std::chrono::steady_clock::now();
    client.auto_reconnect(false);
    client.set_close_timeout(std::chrono::milliseconds(300));
    client.on_connect([&] { client.close(); });
    client.on_close([&](uint16_t code, std::string_view reason) {
        ++r.calls;
        r.code   = code;
        r.reason = std::string(reason);
        r.at_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    });
    client.connect(url_of(srv));
    settle(loop, wait_ms);
    return r;
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("ws_close_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    test_refused_upgrade("403 answer", "HTTP/1.1 403 Forbidden\r\n\r\n", false, "upgrade failed");
    test_refused_upgrade("101 with a wrong Sec-WebSocket-Accept", {}, true, "invalid Sec-WebSocket-Accept");

    {
        const auto r = close_with(Peer::ignore_close, 1500);
        check(r.calls == 1 && r.code == 1006 && r.reason == "close timeout",
              "silent server: on_close(1006, 'close timeout') once, got " + std::to_string(r.calls) + " call(s), code "
                  + std::to_string(r.code) + " '" + r.reason + "'");
        check(r.calls == 1 && r.at_ms < 1200,
              "silent server: on_close within the close timeout, at " + std::to_string(r.at_ms) + " ms");
    }
    {
        const auto r = close_with(Peer::drop_on_close, 1200);
        check(r.calls == 1 && r.code == 1006, "server drops the connection on Close: on_close(1006) once, got "
                                                  + std::to_string(r.calls) + " call(s), code " + std::to_string(r.code));
    }
    {
        const auto r = close_with(Peer::echo_close, 1000);
        check(r.calls == 1 && r.code == 1000, "server answers Close: on_close(1000) once and no later call, got "
                                                  + std::to_string(r.calls) + " call(s), code " + std::to_string(r.code));
    }

    std::printf("ws_close_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
