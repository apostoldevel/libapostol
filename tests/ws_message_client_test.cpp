// ws_message_client_test — WsMessageClient, in this process.
//
//   1. the program links: WsMessage::to_json() and from_json() are defined
//      once, and a message of each type serialises and reads back as written;
//   2. send() with a response handler and a short timeout to a server that
//      never answers: the on_error handler gets "response timeout for <id>",
//      the response handler is not called, and the error comes once.
//
// Usage: ws_message_client_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/ws_message_client.hpp"

#include <signal.h>

#include <string>
#include <vector>

using namespace apostol;
using namespace nettest;

#ifndef WITH_SSL

int main()
{
    std::printf("ws_message_client_test: SKIP built without WITH_SSL\n");
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
        std::printf("ws_message_client_test: FAIL %s\n", what.c_str());
    }
}

void test_codec()
{
    WsMessage req;
    req.id      = "7";
    req.action  = "ping";
    req.payload = {{"a", 1}};
    const auto back = WsMessage::from_json(req.to_json());
    check(back.id == "7" && back.action == "ping" && back.payload["a"] == 1 && back.type == WsMessage::Type::Request,
          "request: to_json()/from_json() round trip, got '" + req.to_json() + "'");

    WsMessage err;
    err.type              = WsMessage::Type::Error;
    err.id                = "8";
    err.error_code        = "E1";
    err.error_description = "bad";
    const auto eb         = WsMessage::from_json(err.to_json());
    check(eb.type == WsMessage::Type::Error && eb.error_code == "E1" && eb.error_description == "bad",
          "error: to_json()/from_json() round trip, got '" + err.to_json() + "'");

    WsMessage resp;
    resp.type = WsMessage::Type::Response;
    resp.id   = "9";
    check(WsMessage::from_json(resp.to_json()).type == WsMessage::Type::Response,
          "response: to_json()/from_json() round trip, got '" + resp.to_json() + "'");
}

void test_response_timeout()
{
    Server srv([](Conn& c, int) {
        if (!ws_handshake(c))
            return;
        std::string b;   // reads, never answers
        while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
    });

    EventLoop                loop;
    WsMessageClient          client(loop);
    std::vector<std::string> errors;
    int                      responses = 0;
    std::string              sent_id;
    client.on_error([&](std::string_view e) { errors.emplace_back(e); });
    client.on_connect([&] {
        WsMessage m;
        m.id     = "req-1";
        m.action = "ask";
        sent_id  = m.id;
        client.send(m, [&](const WsMessage&) { ++responses; }, std::chrono::milliseconds(200));
    });
    client.connect("ws://127.0.0.1:" + std::to_string(srv.port()) + "/");
    run_until(loop, [&] { return !errors.empty(); }, 3000);
    settle(loop, 400);   // nothing else comes
    check(errors.size() == 1 && errors[0] == "response timeout for req-1",
          "unanswered request: on_error 'response timeout for req-1' once, got " + std::to_string(errors.size())
              + " error(s), first '" + (errors.empty() ? "" : errors[0]) + "'");
    check(responses == 0, "unanswered request: the response handler is not called");
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    test_codec();

    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("ws_message_client_test: SKIP no listener on 127.0.0.1\n");
            return g_failed == 0 ? 77 : 1;
        }
    }
    test_response_timeout();

    std::printf("ws_message_client_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
