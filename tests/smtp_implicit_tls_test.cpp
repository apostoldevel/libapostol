// smtp_implicit_tls_test — SmtpClient on port 465 (TLS from the first byte)
// against a relay of its own on 127.0.0.1. Port 465 cannot be bound without
// privileges, so this executable defines connect(): a connection to
// 127.0.0.1:465 is sent to the relay's ephemeral port, every other one is
// left alone. Certificates are made at run time; only the one Trust names is
// in the client's default trust store (SSL_CERT_FILE).
//
//   1. a trusted certificate naming IP:127.0.0.1: the relay sees EHLO, AUTH,
//      MAIL, RCPT, DATA over TLS (no STARTTLS) and the message is delivered;
//   2. a trusted certificate naming DNS:localhost, the client dialling
//      "localhost" (run only where it resolves to 127.0.0.1 first): delivered;
//   3. a trusted certificate naming another host (DNS:other.test): the
//      handshake fails, the message ends in on_error ("SSL_connect(): ...")
//      and the relay's TLS accept ends without a session — no command read;
//   4. a certificate outside the trust store (naming IP:127.0.0.1): the same.
//   In 3 and 4 the relay must have accepted the connection — the refusal is
//   the handshake's, not a dial that never arrived.
//
// The redirect and SSL_CERT_FILE are process-wide: the cases run one at a
// time, one client each.
//
// Usage: smtp_implicit_tls_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/smtp_client.hpp"

#include <dlfcn.h>
#include <netdb.h>
#include <signal.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace apostol;
using namespace nettest;

#ifndef WITH_SSL

int main()
{
    std::printf("smtp_implicit_tls_test: SKIP built without WITH_SSL\n");
    return 77;
}

#else

namespace
{

std::atomic<std::uint16_t> g_relay_port{0};   // where 127.0.0.1:465 goes
std::atomic<int>           g_redirected{0};

} // namespace

// The client side of the test dials 127.0.0.1:465; the relay listens on an
// ephemeral port. Every other connect() is passed through unchanged.
extern "C" int connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    using connect_fn         = int (*)(int, const struct sockaddr*, socklen_t);
    static connect_fn real   = reinterpret_cast<connect_fn>(::dlsym(RTLD_NEXT, "connect"));
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    const std::uint16_t port = g_relay_port.load();
    if (port != 0 && addr && addr->sa_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        sockaddr_in a;
        std::memcpy(&a, addr, sizeof a);
        if (a.sin_port == htons(465) && a.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            a.sin_port = htons(port);
            g_redirected.fetch_add(1);
            return real(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
        }
    }
    return real(fd, addr, len);
}

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("smtp_implicit_tls_test: FAIL %s\n", what.c_str());
    }
}

// The relay: TLS from the first byte, then an SMTP session; what it read.
class Relay
{
public:
    explicit Relay(const Identity& id)
        : server_([this, &id](Conn& c, int) { session(c, id); })
    {}

    bool          ok() const { return server_.ok(); }
    std::uint16_t port() const { return server_.port(); }
    int           connections() const { return server_.accepted(); }
    int           tls_ok() const { return tls_ok_.load(); }
    int           tls_failed() const { return tls_failed_.load(); }

    std::vector<std::string> commands() const
    {
        std::lock_guard<std::mutex> g(m_);
        return cmds_;
    }
    bool saw(const std::string& prefix) const
    {
        for (const auto& c : commands())
            if (c.rfind(prefix, 0) == 0)
                return true;
        return false;
    }

private:
    void log(const std::string& s)
    {
        std::lock_guard<std::mutex> g(m_);
        cmds_.push_back(s);
    }

    static bool next_line(Conn& c, std::string& buf, std::string& line)
    {
        while (buf.find("\r\n") == std::string::npos)
            if (c.read(buf, 3000) != Rd::data)
                return false;
        const auto p = buf.find("\r\n");
        line         = buf.substr(0, p);
        buf.erase(0, p + 2);
        return true;
    }

    void session(Conn& c, const Identity& id)
    {
        if (!c.start_tls(id.ctx())) {
            tls_failed_.fetch_add(1);
            return;
        }
        tls_ok_.fetch_add(1);
        if (!c.write("220 relay ready\r\n"))
            return;

        std::string buf, line;
        bool in_data = false;
        while (!c.stopping()) {
            if (in_data) {
                while (buf.find("\r\n.\r\n") == std::string::npos)
                    if (c.read(buf, 3000) != Rd::data)
                        return;
                buf.erase(0, buf.find("\r\n.\r\n") + 5);
                in_data = false;
                log("<message>");
                c.write("250 queued\r\n");
                continue;
            }
            if (!next_line(c, buf, line))
                return;
            log(line);
            if (line.rfind("EHLO", 0) == 0)
                c.write("250 relay\r\n");
            else if (line == "AUTH LOGIN")
                c.write("334 VXNlcm5hbWU6\r\n");
            else if (line == "dXNlckB4LnRlc3Q=")
                c.write("334 UGFzc3dvcmQ6\r\n");
            else if (line == "c2VjcmV0")
                c.write("235 ok\r\n");
            else if (line.rfind("MAIL FROM:", 0) == 0 || line.rfind("RCPT TO:", 0) == 0)
                c.write("250 ok\r\n");
            else if (line == "DATA") {
                c.write("354 go\r\n");
                in_data = true;
            } else if (line == "QUIT") {
                c.write("221 bye\r\n");
                return;
            } else
                c.write("500 what\r\n");
        }
    }

    mutable std::mutex       m_;
    std::vector<std::string> cmds_;
    std::atomic<int>         tls_ok_{0};
    std::atomic<int>         tls_failed_{0};
    Server                   server_;
};

struct Outcome
{
    bool        done{false};
    std::string error;
};

// One message to the relay through 127.0.0.1:465, with AUTH LOGIN.
Outcome send_one(Relay& relay, const std::string& host = "127.0.0.1")
{
    g_relay_port.store(relay.port());
    const int before = g_redirected.load();

    EventLoop  loop;
    SmtpConfig cfg;
    cfg.host     = host;
    cfg.port     = 465;
    cfg.username = "user@x.test";
    cfg.password = "secret";
    SmtpClient smtp(loop, cfg);

    Outcome out;
    auto&   m = smtp.add_message();
    m.from    = "from@x.test";
    m.to      = {"to@x.test"};
    m.subject = "s";
    m.body    = "hello";
    m.on_done  = [&out](const SmtpMessage&) { out.done = true; };
    m.on_error = [&out](const SmtpMessage&, std::string_view e) { out.error = std::string(e); };
    smtp.send_mail();
    run_until(loop, [&out] { return out.done || !out.error.empty(); }, 5000);

    check(g_redirected.load() > before, "the dial to 127.0.0.1:465 reached the relay's port");
    g_relay_port.store(0);
    return out;
}

bool localhost_is_ipv4_first()
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* r       = nullptr;
    if (::getaddrinfo("localhost", "465", &hints, &r) != 0 || !r)
        return false;
    bool v4 = false;
    if (r->ai_family == AF_INET) {
        sockaddr_in a;
        std::memcpy(&a, r->ai_addr, sizeof a);
        v4 = a.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    }
    ::freeaddrinfo(r);
    return v4;
}

void test_trusted(const std::string& san, const std::string& host)
{
    const Identity id(san);
    const Trust    trust(id.pem());
    Relay          relay(id);
    check(relay.ok(), "listener");

    const auto out = send_one(relay, host);
    check(out.done && out.error.empty(),
          "trusted " + san + " certificate, dialled '" + host + "': delivered, error '" + out.error + "'");
    check(relay.saw("EHLO") && relay.saw("AUTH LOGIN") && relay.saw("MAIL FROM:") && relay.saw("RCPT TO:")
              && relay.saw("<message>"),
          "trusted certificate: the relay saw EHLO, AUTH LOGIN, MAIL, RCPT and the message");
    check(!relay.saw("STARTTLS"), "port 465: no STARTTLS command");
}

void test_refused(const char* label, const Identity& served, const Identity& trusted)
{
    const Trust trust(trusted.pem());
    Relay       relay(served);
    check(relay.ok(), "listener");

    const auto out = send_one(relay);
    check(!out.done && !out.error.empty(),
          std::string(label) + ": refused with on_error, delivered " + (out.done ? "yes" : "no") + ", error '"
              + out.error + "'");
    check(out.error.rfind("SSL_connect()", 0) == 0,
          std::string(label) + ": the error is the client's handshake, error '" + out.error + "'");
    check(relay.connections() >= 1, std::string(label) + ": the relay accepted the connection");

    // The relay's side of the handshake ends on its own thread: wait for it.
    for (int i = 0; i < 150 && relay.tls_ok() + relay.tls_failed() == 0; ++i)
        std::this_thread::sleep_for(20ms);
    check(relay.tls_ok() == 0 && relay.tls_failed() >= 1,
          std::string(label) + ": the relay's TLS accept failed, ok " + std::to_string(relay.tls_ok()) + ", failed "
              + std::to_string(relay.tls_failed()));
    check(relay.commands().empty(), std::string(label) + ": the relay read no command, it read "
                                        + std::to_string(relay.commands().size()));
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("smtp_implicit_tls_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    test_trusted("IP:127.0.0.1", "127.0.0.1");
    if (localhost_is_ipv4_first())
        test_trusted("DNS:localhost", "localhost");
    else
        std::printf("smtp_implicit_tls_test: 'localhost' does not resolve to 127.0.0.1 first, case 2 not run\n");
    {
        const Identity other("DNS:other.test");
        test_refused("trusted certificate for another host", other, other);
    }
    {
        const Identity served("IP:127.0.0.1");
        const Identity trusted("IP:127.0.0.1");
        test_refused("certificate outside the trust store", served, trusted);
    }

    std::printf("smtp_implicit_tls_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
