// tcp_client_test — a TcpClient its own handler destroys. The owner of a
// client may let go of it from any of its handlers; the client must not touch
// itself once the handler returns. Each client lives in sealed pages (Sealed):
// a touch after the handler destroyed it stops the test with a FAIL line.
//
//   1. on_connect destroys the client (a listener that accepts);
//   2. on_data destroys it (the peer writes as soon as it accepts);
//   3. on_close destroys it (the peer closes as soon as it accepts);
//   4. on_error destroys it — the connection refused (a closed port);
//   5. on_error destroys it inside connect() — a host that does not resolve
//      is refused before connect() returns (the empty name: refused by the
//      resolver itself, no lookup on the network);
//   6. on_error destroys it from its idle timer (a peer that stays silent).
//
// Usage: tcp_client_test [connect|data|close|refused|resolve|idle] — one case, or
// all without an argument (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/tcp_client.hpp"

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
        std::printf("tcp_client_test: FAIL %s\n", what.c_str());
    }
}

void test_destroyed_on_connect()
{
    Server server([](Conn& c, int) {
        std::string buf;
        while (!c.stopping() && c.read(buf, 200) != Rd::eof) {}
    });
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_connect", loop);
    bool              connected = false;
    tcp->on_connect([&] {
        connected = true;
        tcp.destroy();
    });
    tcp->on_error([&](std::string_view e) { check(false, "on_connect case: on_error '" + std::string(e) + "'"); });
    tcp->connect("127.0.0.1", server.port());
    run_until(loop, [&] { return connected; }, 3000);
    settle(loop, 100);
    check(connected && tcp.destroyed(), "on_connect fired and destroyed the client");
}

void test_destroyed_on_data()
{
    Server server([](Conn& c, int) {
        c.write("hello");
        std::string buf;
        while (!c.stopping() && c.read(buf, 200) != Rd::eof) {}
    });
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_data", loop);
    std::string       got;
    tcp->on_data([&](const char* d, size_t n) {
        got.assign(d, n);
        tcp.destroy();
    });
    tcp->connect("127.0.0.1", server.port());
    run_until(loop, [&] { return tcp.destroyed(); }, 3000);
    settle(loop, 100);
    check(got == "hello" && tcp.destroyed(), "on_data got the peer's bytes and destroyed the client, got '" + got + "'");
}

void test_destroyed_on_close()
{
    Server server([](Conn&, int) {});   // returns at once: the connection is closed
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_close", loop);
    bool              closed = false;
    tcp->on_close([&] {
        closed = true;
        tcp.destroy();
    });
    tcp->connect("127.0.0.1", server.port());
    run_until(loop, [&] { return closed; }, 3000);
    settle(loop, 100);
    check(closed && tcp.destroyed(), "on_close fired on the peer's close and destroyed the client");
}

void test_destroyed_on_refused()
{
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_error (connection refused)", loop);
    std::string       error;
    tcp->on_error([&](std::string_view e) {
        error = std::string(e);
        tcp.destroy();
    });
    const ClosedPort closed;
    tcp->connect("127.0.0.1", closed.port());
    run_until(loop, [&] { return tcp.destroyed(); }, 3000);
    settle(loop, 100);
    check(error.find("connect failed") != std::string::npos && tcp.destroyed(),
          "connection refused: on_error 'connect failed…' destroyed the client, got '" + error + "'");
}

void test_destroyed_on_resolve()
{
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_error inside connect()", loop);
    std::string       error;
    tcp->on_error([&](std::string_view e) {
        error = std::string(e);
        tcp.destroy();
    });
    tcp->connect("", 25);
    check(error.find("getaddrinfo") != std::string::npos && tcp.destroyed(),
          "a host that does not resolve: on_error inside connect() destroyed the client, got '" + error + "'");
    settle(loop, 50);
}

void test_destroyed_on_idle()
{
    Server server([](Conn& c, int) {
        std::string buf;
        while (!c.stopping() && c.read(buf, 200) != Rd::eof) {}
    });
    EventLoop         loop;
    Sealed<TcpClient> tcp("destroyed by on_error (idle timer)", loop);
    std::string       error;
    tcp->set_idle_timeout(std::chrono::milliseconds(100));
    tcp->on_error([&](std::string_view e) {
        error = std::string(e);
        tcp.destroy();
    });
    tcp->connect("127.0.0.1", server.port());
    run_until(loop, [&] { return tcp.destroyed(); }, 3000);
    settle(loop, 100);
    check(error == "idle timeout" && tcp.destroyed(), "idle timer: on_error destroyed the client, got '" + error + "'");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    bool       ran = false;
    const auto run = [&](const char* name, void (*fn)()) {
        if (only.empty() || only == name) {
            ran = true;
            fn();
        }
    };

    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("tcp_client_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    run("connect", test_destroyed_on_connect);
    run("data", test_destroyed_on_data);
    run("close", test_destroyed_on_close);
    run("refused", test_destroyed_on_refused);
    run("resolve", test_destroyed_on_resolve);
    run("idle", test_destroyed_on_idle);
    if (!ran) {
        std::printf("tcp_client_test: no case '%s'\n", only.c_str());
        return 2;
    }

    std::printf("tcp_client_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
