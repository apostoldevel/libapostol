// tls_client_test — TLS of TcpClient and of HttpProxy, in this process, against
// TLS and plain servers of its own on 127.0.0.1 (self-signed certificates made
// at run time, trusted through SSL_CERT_FILE).
//
//   1. a verified TcpClient connection checks the host name it dialled:
//      - a certificate naming DNS:localhost is accepted for "localhost" and
//        refused ("SSL_connect(): ...") for "127.0.0.1";
//      - a certificate naming IP:127.0.0.1 is accepted for "127.0.0.1" and
//        refused for "localhost";
//   2. start_tls() upgrades one connection only: after close(), the next
//      connect() of the same client is plain text, and data arrives as such;
//   3. HttpProxy to a TLS upstream verifies by default (an upstream whose
//      certificate is not trusted fails the forward: connect failure, 502 when
//      no failure handler is given) and set_tls(true, false) lifts the check;
//      a trusted upstream answers 200 either way.
//
// Usage: tls_client_test (exit 77 — skipped — without a loopback listener, or
// where "localhost" does not resolve to 127.0.0.1 first)

#include "net_test_util.hpp"

#include "apostol/http_proxy.hpp"
#include "apostol/tcp_client.hpp"

#include <netdb.h>
#include <signal.h>

#include <string>

using namespace apostol;
using namespace nettest;

#ifndef WITH_SSL

int main()
{
    std::printf("tls_client_test: SKIP built without WITH_SSL\n");
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
        std::printf("tls_client_test: FAIL %s\n", what.c_str());
    }
}

bool localhost_is_ipv4_first()
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* r       = nullptr;
    if (::getaddrinfo("localhost", "80", &hints, &r) != 0 || !r)
        return false;
    const bool v4 = r->ai_family == AF_INET;
    ::freeaddrinfo(r);
    return v4;
}

// A TLS server that completes the handshake and then holds the connection.
Server::Handler tls_holder(const Identity& id)
{
    return [&id](Conn& c, int) {
        if (!c.start_tls(id.ctx()))
            return;
        std::string b;
        while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
    };
}

struct Dial
{
    bool        connected{false};
    std::string error;
};

// connect(host, port) with verified TLS from the first byte; the outcome.
Dial dial(const std::string& host, std::uint16_t port)
{
    EventLoop loop;
    Dial      out;
    TcpClient c(loop);
    c.enable_tls(true);
    c.on_connect([&out] { out.connected = true; });
    c.on_error([&out](std::string_view e) { out.error = std::string(e); });
    c.connect(host, port);
    run_until(loop, [&out] { return out.connected || !out.error.empty(); }, 3000);
    return out;
}

void test_host_name()
{
    {
        const Identity id("DNS:localhost");
        const Trust    trust(id.pem());
        Server         srv(tls_holder(id));
        check(srv.ok(), "listener");

        const auto by_name = dial("localhost", srv.port());
        check(by_name.connected && by_name.error.empty(),
              "DNS:localhost certificate, dialled 'localhost': connected, error '" + by_name.error + "'");

        const auto by_ip = dial("127.0.0.1", srv.port());
        check(!by_ip.connected && by_ip.error.rfind("SSL_connect()", 0) == 0,
              "DNS:localhost certificate, dialled 127.0.0.1: refused with SSL_connect(), connected "
                  + std::string(by_ip.connected ? "yes" : "no") + ", error '" + by_ip.error + "'");
    }
    {
        const Identity id("IP:127.0.0.1");
        const Trust    trust(id.pem());
        Server         srv(tls_holder(id));

        const auto by_ip = dial("127.0.0.1", srv.port());
        check(by_ip.connected && by_ip.error.empty(),
              "IP:127.0.0.1 certificate, dialled 127.0.0.1: connected, error '" + by_ip.error + "'");

        const auto by_name = dial("localhost", srv.port());
        check(!by_name.connected && by_name.error.rfind("SSL_connect()", 0) == 0,
              "IP:127.0.0.1 certificate, dialled 'localhost': refused with SSL_connect(), error '"
                  + by_name.error + "'");
    }
    {
        const Identity id("DNS:other.test");
        const Trust    trust(id.pem());
        Server         srv(tls_holder(id));

        const auto by_name = dial("localhost", srv.port());
        check(!by_name.connected && by_name.error.rfind("SSL_connect()", 0) == 0,
              "DNS:other.test certificate, dialled 'localhost': refused with SSL_connect(), error '"
                  + by_name.error + "'");
    }
}

// start_tls() on one connection; the next connect() of the same client plain.
void test_starttls_per_connection()
{
    const Identity id("IP:127.0.0.1");
    const Trust    trust(id.pem());
    Server         srv([&id](Conn& c, int index) {
        if (index == 0) {
            if (!c.write("hello\r\n") || !c.start_tls(id.ctx()))
                return;
            std::string b;
            if (read_until(c, b, "ping"))
                c.write("pong");
            while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
        } else {
            c.write("plain-hello\r\n");
            std::string b;
            while (!c.stopping() && c.read(b, 200) != Rd::eof) {}
        }
    });

    EventLoop   loop;
    TcpClient   c(loop);
    int         connects = 0;
    bool        upgraded = false;
    std::string buf;
    std::string error;
    c.on_connect([&] {
        ++connects;
        if (upgraded && connects == 2)
            c.send("ping");
    });
    c.on_data([&](const char* d, std::size_t n) {
        buf.append(d, n);
        if (!upgraded && buf.find("hello\r\n") != std::string::npos) {
            upgraded = true;
            buf.clear();
            c.start_tls();
        }
    });
    c.on_error([&error](std::string_view e) { error = std::string(e); });

    c.connect("127.0.0.1", srv.port());
    const bool first = run_until(loop, [&] { return buf == "pong" || !error.empty(); }, 4000);
    check(first && buf == "pong" && connects == 2 && error.empty(),
          "STARTTLS: upgrade and an exchange over TLS (connects " + std::to_string(connects) + ", got '" + buf
              + "', error '" + error + "')");

    c.close();
    buf.clear();
    error.clear();
    c.connect("127.0.0.1", srv.port());
    run_until(loop, [&] { return buf.find("plain-hello\r\n") != std::string::npos || !error.empty(); }, 4000);
    check(connects == 3 && error.empty(),
          "STARTTLS: the next connect() of the client reaches on_connect in plain text (connects "
              + std::to_string(connects) + ", error '" + error + "')");
    check(buf == "plain-hello\r\n", "STARTTLS: the next connection's data arrives as text, got '" + buf + "'");
}

// ─── HttpProxy to a TLS upstream ─────────────────────────────────────────────

struct Fwd
{
    int         status{0};
    bool        failed{false};
    int         reason{-1};
    std::string message;
    bool        done() const { return status != 0 || failed; }
};

Server::Handler https_upstream(const Identity& id)
{
    return [&id](Conn& c, int) {
        if (!c.start_tls(id.ctx()))
            return;
        std::string req;
        if (!read_until(c, req, "\r\n\r\n"))
            return;
        c.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        c.close_now();
    };
}

// forward() one GET to "localhost":port with set_tls(true, verify); with
// `handler`, a failure is a ForwardFailure, without, a 502 answer.
Fwd forward_once(std::uint16_t port, bool verify, bool handler)
{
    EventLoop   loop;
    Fwd         out;
    HttpProxy   p(loop, "localhost", port);
    p.set_tls(true, verify);
    HttpRequest req;
    req.method = "GET";
    req.path   = "/";
    auto on_response = [&out](const HttpResponse& r) { out.status = r.status_code(); };
    if (handler)
        p.forward(req, on_response, [&out](const HttpProxy::ForwardFailure& f) {
            out.failed  = true;
            out.reason  = static_cast<int>(f.reason);
            out.message = std::string(f.message);
        });
    else
        p.forward(req, on_response);
    run_until(loop, [&out] { return out.done(); }, 4000);
    return out;
}

void test_proxy_tls()
{
    const Identity upstream("DNS:localhost");
    const Identity stranger("DNS:localhost");   // same name, another authority
    Server         srv(https_upstream(upstream));

    {
        const Trust trust(upstream.pem());
        const auto  r = forward_once(srv.port(), true, true);
        check(r.status == 200 && !r.failed, "proxy, trusted upstream: 200, got " + std::to_string(r.status)
                                                + (r.failed ? " failure '" + r.message + "'" : ""));
    }
    {
        const Trust trust(stranger.pem());
        const auto  r = forward_once(srv.port(), true, true);
        check(r.failed && r.status == 0 && r.reason == static_cast<int>(HttpProxy::ForwardFailure::Reason::connect),
              "proxy, untrusted upstream: connect failure, got status " + std::to_string(r.status) + " failed "
                  + (r.failed ? "yes" : "no") + " '" + r.message + "'");

        const auto r502 = forward_once(srv.port(), true, false);
        check(r502.status == 502, "proxy, untrusted upstream, no failure handler: 502, got "
                                      + std::to_string(r502.status));

        const auto off = forward_once(srv.port(), false, true);
        check(off.status == 200 && !off.failed,
              "proxy, untrusted upstream, set_tls(true, false): 200, got " + std::to_string(off.status)
                  + (off.failed ? " failure '" + off.message + "'" : ""));
    }
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("tls_client_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }
    if (!localhost_is_ipv4_first()) {
        std::printf("tls_client_test: SKIP 'localhost' does not resolve to 127.0.0.1 first\n");
        return 77;
    }

    test_host_name();
    test_starttls_per_connection();
    test_proxy_tls();

    std::printf("tls_client_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
