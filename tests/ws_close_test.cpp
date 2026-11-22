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
//      would have elapsed;
//   5. close() with a server that resets the connection on the Close frame
//      (TCP RST, a socket error rather than an end of stream): on_close(1006)
//      once, and no second call when the close timeout would have elapsed;
//   6. with auto-reconnect on, a server that sends Close 1001 and then resets
//      the connection (the client's echo of the Close lands on a socket the
//      server has already released): the socket error that follows the Close
//      leaves the scheduled reconnect in place, and the client dials again.
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

enum class Peer { reset_on_close, ignore_close, drop_on_close, echo_close };

// Connect, call close() once connected, and watch on_close for `wait_ms`.
CloseResult close_with(Peer peer, int wait_ms)
{
    Server srv([peer](Conn& c, int) {
        if (!ws_handshake(c))
            return;
        if (peer == Peer::reset_on_close) {
            // Wait for the client's Close frame without reading it: a socket
            // closed with unread data in its receive queue answers with RST.
            c.wait(POLLIN, 3000);
            c.close_now();
            return;
        }
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

struct ReconnectResult
{
    int                      connects{0};
    std::vector<uint16_t>    closes;
    std::vector<std::string> errors;
    int                      accepted{0};
    bool                     reset{false};   // the echo was queued unread when the server closed
};

// Close 1001 from the server, then a reset; the second connection is held
// open. Watch the client for `wait_ms`.
ReconnectResult close_then_reset(int wait_ms)
{
    std::atomic<bool> reset{false};
    Server srv([&reset](Conn& c, int index) {
        if (!ws_handshake(c))
            return;
        if (index == 0) {
            c.write(std::string("\x88\x0C\x03\xE9going away", 14));   // Close, 1001
            // Wait for the client's echo without reading it: a socket closed
            // with unread data in its receive queue answers with RST.
            reset = c.wait(POLLIN, 3000);
            c.close_now();
            return;
        }
        std::string b;
        while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
    });

    EventLoop       loop;
    WsClient        client(loop);
    ReconnectResult r;
    client.auto_reconnect(true);
    client.on_connect([&] { ++r.connects; });
    client.on_close([&](uint16_t code, std::string_view) { r.closes.push_back(code); });
    client.on_error([&](std::string_view e) { r.errors.emplace_back(e); });
    client.connect(url_of(srv));
    run_until(loop, [&] { return r.connects >= 2; }, wait_ms);
    r.accepted = srv.accepted();
    r.reset    = reset.load();
    client.auto_reconnect(false);
    client.close();
    settle(loop, 200);
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
        const auto r = close_with(Peer::reset_on_close, 1200);
        check(r.calls == 1 && r.code == 1006, "server resets the connection on Close: on_close(1006) once, got "
                                                  + std::to_string(r.calls) + " call(s), code " + std::to_string(r.code));
    }
    {
        const auto r = close_with(Peer::echo_close, 1000);
        check(r.calls == 1 && r.code == 1000, "server answers Close: on_close(1000) once and no later call, got "
                                                  + std::to_string(r.calls) + " call(s), code " + std::to_string(r.code));
    }

    {
        const auto r = close_then_reset(4000);
        check(r.reset, "Close 1001 then reset: the client's echo reached the server before it closed (no RST otherwise)");
        check(!r.closes.empty() && r.closes[0] == 1001,
              "Close 1001 then reset: on_close(1001) first, got " + std::to_string(r.closes.size()) + " call(s)"
                  + (r.closes.empty() ? std::string() : ", code " + std::to_string(r.closes[0])));
        check(r.connects == 2 && r.accepted == 2,
              "Close 1001 then reset: the client dialled again, connects " + std::to_string(r.connects)
                  + ", connections " + std::to_string(r.accepted) + ", errors " + std::to_string(r.errors.size())
                  + (r.errors.empty() ? std::string() : " ('" + r.errors[0] + "')"));
    }

    std::printf("ws_close_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
