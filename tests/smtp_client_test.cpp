// smtp_client_test — SmtpClient against a relay of its own on 127.0.0.1 (a
// port other than 465, so the client takes the STARTTLS path; the relay
// completes STARTTLS with a certificate made at run time and trusted through
// SSL_CERT_FILE; the client dials "127.0.0.1", the certificate names it).
//
//   1. envelope addresses: a recipient or a sender with a control byte (CR LF
//      among them), an empty recipient string, or a message with no recipient,
//      is refused with its own on_error ("invalid envelope recipient",
//      "invalid envelope sender", "no envelope recipient") and the relay never
//      sees it — no injected command; a good message queued with them is
//      delivered (RCPT, DATA, on_done); when every message is refused nothing
//      is dialled;
//   2. data after the 220 of STARTTLS, in the same write, is refused ("data
//      after the 220");
//   3. a reply line that never ends is refused once it is longer than 4096
//      bytes ("longer than 4096 bytes") — the relay sends 1 MiB without CRLF;
//   4. one SmtpClient sends twice (the second send_mail() after the first
//      finished): both go through STARTTLS and are delivered;
//   5. AUTH LOGIN carries the user name and the password as base64;
//   6. the message's handler destroys the client — on_error of a refused
//      greeting, of a connection the relay closes, of a refused connection,
//      and on_done of a delivered message: the client lives in sealed pages
//      (Sealed) and is not touched after the handler returns; the message a
//      handler is given stays readable after it destroyed the client;
//   7. on_error sends again: the failed message is retried, the one delivered
//      before it in the same session is not sent twice.
//
// Usage: smtp_client_test [greeting|error-message|hangup|refused|done|message|retry] — one case of 6–7, or
// every case without an argument (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/smtp_client.hpp"

#include <signal.h>

#include <mutex>
#include <string>
#include <vector>

using namespace apostol;
using namespace nettest;

#ifndef WITH_SSL

int main()
{
    std::printf("smtp_client_test: SKIP built without WITH_SSL\n");
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
        std::printf("smtp_client_test: FAIL %s\n", what.c_str());
    }
}

enum class Mode { normal, glued_220, endless_line, refuse_greeting, hangup };

// The relay: what each connection said, in order, and how it behaves.
class Relay
{
public:
    explicit Relay(Mode mode, const Identity& id)
        : mode_(mode)
        , server_([this, &id](Conn& c, int) { session(c, id); })
    {}

    bool          ok() const { return server_.ok(); }
    std::uint16_t port() const { return server_.port(); }
    int           connections() const { return server_.accepted(); }

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
    bool saw_containing(const std::string& part) const
    {
        for (const auto& c : commands())
            if (c.find(part) != std::string::npos)
                return true;
        return false;
    }
    int count(const std::string& prefix) const
    {
        int n = 0;
        for (const auto& c : commands())
            if (c.rfind(prefix, 0) == 0)
                ++n;
        return n;
    }

private:
    void log(const std::string& s)
    {
        std::lock_guard<std::mutex> g(m_);
        cmds_.push_back(s);
    }

    // Take one command line (or, in DATA, the whole message) from the stream.
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
        std::string buf, line;
        if (mode_ == Mode::hangup)
            return;   // closed before a greeting
        if (mode_ == Mode::refuse_greeting) {
            c.write("554 no service\r\n221 bye\r\n");   // one write: the reply text must not take the next line's bytes
            while (!c.stopping() && c.read(buf, 200) != Rd::eof) {}
            return;
        }
        if (!c.write("220 relay ready\r\n"))
            return;

        if (mode_ == Mode::endless_line) {
            if (!next_line(c, buf, line))
                return;
            log(line);
            // No reply to EHLO: a line that does not end.
            const std::string chunk(65536, 'A');
            for (int i = 0; i < 16 && c.write(chunk, 2000); ++i) {}
            while (!c.stopping() && c.read(buf, 200) != Rd::eof) {}
            return;
        }

        if (!next_line(c, buf, line))
            return;
        log(line);   // EHLO
        c.write("250 relay\r\n");

        if (!next_line(c, buf, line))
            return;
        log(line);   // STARTTLS
        if (mode_ == Mode::glued_220)
            c.write("220 go\r\n250 x\r\n");
        else
            c.write("220 go\r\n");
        buf.clear();
        if (!c.start_tls(id.ctx()))
            return;

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
            else if (line.rfind("RCPT TO:<fail@", 0) == 0)
                c.write("550 no such user\r\n");
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

    Mode                     mode_;
    mutable std::mutex       m_;
    std::vector<std::string> cmds_;
    Server                   server_;
};

struct Sent
{
    std::vector<std::string> done;                        // subjects delivered
    std::vector<std::pair<std::string, std::string>> failed;   // subject, error
};

SmtpMessage& queue(SmtpClient& smtp, Sent& sent, const std::string& subject, const std::string& from,
                   std::vector<std::string> to)
{
    auto& m   = smtp.add_message();
    m.from    = from;
    m.to      = std::move(to);
    m.subject = subject;
    m.body    = "hello";
    m.on_done = [&sent, subject](const SmtpMessage&) { sent.done.push_back(subject); };
    m.on_error = [&sent, subject](const SmtpMessage&, std::string_view e) {
        sent.failed.emplace_back(subject, std::string(e));
    };
    return m;
}

SmtpConfig config(std::uint16_t port, bool auth = false)
{
    SmtpConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    if (auth) {
        cfg.username = "user@x.test";
        cfg.password = "secret";
    }
    return cfg;
}

std::string error_of(const Sent& s, const std::string& subject)
{
    for (const auto& [sub, e] : s.failed)
        if (sub == subject)
            return e;
    return {};
}

bool delivered(const Sent& s, const std::string& subject)
{
    for (const auto& d : s.done)
        if (d == subject)
            return true;
    return false;
}

void test_envelope(const Identity& id)
{
    Relay relay(Mode::normal, id);
    check(relay.ok(), "relay listener");

    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port(), true));
    queue(smtp, sent, "bad-to", "a@x.test", {"a@x.test>\r\nRCPT TO:<evil@x.test"});
    queue(smtp, sent, "bad-from", "a@x.test>\r\nRCPT TO:<evil@x.test", {"b@x.test"});
    queue(smtp, sent, "no-to", "a@x.test", {});
    queue(smtp, sent, "empty-to", "a@x.test", {""});
    queue(smtp, sent, "good", "a@x.test", {"b@x.test"});
    smtp.send_mail();

    // The refusals are reported before send_mail() returns.
    check(error_of(sent, "bad-to") == "invalid envelope recipient",
          "recipient with CR LF: on_error 'invalid envelope recipient', got '" + error_of(sent, "bad-to") + "'");
    check(error_of(sent, "bad-from") == "invalid envelope sender",
          "sender with CR LF: on_error 'invalid envelope sender', got '" + error_of(sent, "bad-from") + "'");
    check(error_of(sent, "no-to") == "no envelope recipient",
          "no recipient: on_error 'no envelope recipient', got '" + error_of(sent, "no-to") + "'");
    check(error_of(sent, "empty-to") == "invalid envelope recipient",
          "empty recipient string: on_error 'invalid envelope recipient', got '" + error_of(sent, "empty-to") + "'");

    run_until(loop, [&] { return delivered(sent, "good") || !error_of(sent, "good").empty(); }, 5000);
    check(delivered(sent, "good"), "good message queued with the bad ones: delivered, error '"
                                       + error_of(sent, "good") + "'");
    check(!relay.saw_containing("evil"), "the relay never saw the injected command");
    check(relay.count("RCPT TO:") == 1 && relay.saw("RCPT TO:<b@x.test>"),
          "the relay saw exactly the good recipient, RCPT count " + std::to_string(relay.count("RCPT TO:")));
    check(!relay.saw("RCPT TO:<>"), "the relay never saw an empty recipient");
    check(relay.count("MAIL FROM:") == 1, "the relay saw one MAIL FROM, got " + std::to_string(relay.count("MAIL FROM:")));
    check(relay.count("DATA") == 1, "the relay saw one DATA");
    check(sent.failed.size() == 4, "four refusals, got " + std::to_string(sent.failed.size()));

    // Credentials go as base64 (the relay answers 334/235 only to these).
    check(relay.saw("AUTH LOGIN") && relay.saw("dXNlckB4LnRlc3Q=") && relay.saw("c2VjcmV0"),
          "AUTH LOGIN carried the user name and the password as base64");
}

void test_all_refused(const Identity& id)
{
    Relay relay(Mode::normal, id);
    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port()));
    queue(smtp, sent, "bad", "a@x.test", {"a b@x.test"});
    smtp.send_mail();
    check(error_of(sent, "bad") == "invalid envelope recipient", "recipient with a blank: refused");
    check(!smtp.active(), "every message refused: the client is not active");
    settle(loop, 300);
    check(relay.connections() == 0, "every message refused: nothing dialled, connections "
                                        + std::to_string(relay.connections()));
}

void test_glued_220(const Identity& id)
{
    Relay      relay(Mode::glued_220, id);
    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port()));
    queue(smtp, sent, "m", "a@x.test", {"b@x.test"});
    smtp.send_mail();
    run_until(loop, [&] { return !sent.failed.empty() || !sent.done.empty(); }, 3000);
    const auto e = error_of(sent, "m");
    check(e.find("data after the 220") != std::string::npos,
          "data after the 220 of STARTTLS: refused, got error '" + e + "', delivered " + (delivered(sent, "m") ? "yes" : "no"));
}

void test_endless_line(const Identity& id)
{
    Relay      relay(Mode::endless_line, id);
    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port()));
    queue(smtp, sent, "m", "a@x.test", {"b@x.test"});
    smtp.send_mail();
    const auto t0 = std::chrono::steady_clock::now();
    run_until(loop, [&] { return !sent.failed.empty(); }, 3000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const auto e  = error_of(sent, "m");
    check(e.find("longer than 4096 bytes") != std::string::npos,
          "a reply line without CRLF: refused past 4096 bytes, got error '" + e + "' after " + std::to_string(ms) + " ms");
}

void test_two_sends(const Identity& id)
{
    Relay      relay(Mode::normal, id);
    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port()));
    queue(smtp, sent, "first", "a@x.test", {"b@x.test"});
    smtp.send_mail();
    run_until(loop, [&] { return !smtp.active() && (delivered(sent, "first") || !sent.failed.empty()); }, 5000);
    check(delivered(sent, "first"), "first send delivered, error '" + error_of(sent, "first") + "'");

    queue(smtp, sent, "second", "a@x.test", {"c@x.test"});
    smtp.send_mail();
    run_until(loop, [&] { return delivered(sent, "second") || !error_of(sent, "second").empty(); }, 5000);
    check(delivered(sent, "second"), "second send of the same client delivered, error '"
                                         + error_of(sent, "second") + "'");
    check(relay.count("STARTTLS") == 2 && relay.saw("RCPT TO:<c@x.test>"),
          "both sends went through STARTTLS, STARTTLS count " + std::to_string(relay.count("STARTTLS")));
}

// The owner lets go of the client from on_error, as a sender that drops the
// delivery it failed does.
// read_after: the handler reads the message after it destroyed the client.
void test_destroyed_on_error(const char* label, std::uint16_t port, const std::string& expected,
                             bool read_after = false)
{
    EventLoop          loop;
    Sealed<SmtpClient> smtp(label, loop, config(port));
    std::string        error;
    int                calls = 0;
    auto& m   = smtp->add_message();
    m.from    = "a@x.test";
    m.to      = {"b@x.test"};
    m.subject = "m";
    m.body    = "hello";
    m.on_error = [&](const SmtpMessage& msg, std::string_view e) {
        ++calls;
        if (!read_after)
            error = std::string(e) + " / " + msg.subject;
        smtp.destroy();
        if (read_after)
            error = std::string(e) + " / " + msg.subject;
    };
    smtp->send_mail();
    run_until(loop, [&] { return smtp.destroyed(); }, 5000);
    settle(loop, 100);
    check(calls == 1 && error.find(expected) != std::string::npos && error.find(" / m") != std::string::npos,
          std::string(label) + ": on_error once with '" + expected + "', got " + std::to_string(calls)
              + " call(s), '" + error + "'");
}

// read_after: the handler reads the message after it destroyed the client —
// the message it is given is its own, not the client's.
void test_destroyed_on_done(const Identity& id, bool read_after)
{
    Relay              relay(Mode::normal, id);
    EventLoop          loop;
    Sealed<SmtpClient> smtp(read_after ? "the message handed to on_done, read after it destroyed the client"
                                       : "SmtpClient destroyed by on_done",
                            loop, config(relay.port()));
    std::string        done;
    auto& m   = smtp->add_message();
    m.from    = "a@x.test";
    m.to      = {"b@x.test"};
    m.subject = "m";
    m.body    = "hello";
    m.on_done = [&, read_after](const SmtpMessage& msg) {
        if (!read_after)
            done = msg.subject;
        smtp.destroy();
        if (read_after)
            done = msg.subject;
    };
    m.on_error = [&](const SmtpMessage&, std::string_view e) { check(false, "on_done case: on_error '" + std::string(e) + "'"); };
    smtp->send_mail();
    run_until(loop, [&] { return smtp.destroyed(); }, 5000);
    settle(loop, 100);
    check(done == "m", "on_done destroyed the client after delivery, got '" + done + "'");
}

void test_retry_from_on_error(const Identity& id)
{
    Relay      relay(Mode::normal, id);
    EventLoop  loop;
    Sent       sent;
    SmtpClient smtp(loop, config(relay.port()));
    // No on_done on the first: the delivered message stays as it was in the
    // queue, and only the client's own bookkeeping keeps it from going twice.
    queue(smtp, sent, "first", "a@x.test", {"b@x.test"}).on_done = nullptr;
    auto& m    = queue(smtp, sent, "second", "a@x.test", {"fail@x.test"});
    int  tries = 0;
    m.on_error = [&](const SmtpMessage&, std::string_view e) {
        sent.failed.emplace_back("second", std::string(e));
        if (++tries == 1)
            smtp.send_mail();   // once more
    };
    smtp.send_mail();
    run_until(loop, [&] { return tries == 2; }, 8000);
    settle(loop, 200);
    check(tries == 2 && relay.count("<message>") == 1,
          "a retry from on_error: the failed message tried twice (" + std::to_string(tries) + "), one message delivered ("
              + std::to_string(relay.count("<message>")) + ")");
    check(relay.count("RCPT TO:<b@x.test>") == 1 && relay.count("RCPT TO:<fail@x.test>") == 2,
          "the retry did not send the delivered message again: RCPT b " + std::to_string(relay.count("RCPT TO:<b@x.test>"))
              + ", RCPT fail " + std::to_string(relay.count("RCPT TO:<fail@x.test>")));
    check(error_of(sent, "second") == "RCPT TO: 550 no such user",
          "the reply text reaches on_error whole, got '" + error_of(sent, "second") + "'");
}

bool test_destroyed_by_handler(const Identity& id, const std::string& only)
{
    bool       ran  = false;
    const auto want = [&](const char* name) {
        const bool w = only.empty() || only == name;
        ran          = ran || w;
        return w;
    };
    if (want("greeting")) {
        Relay relay(Mode::refuse_greeting, id);
        test_destroyed_on_error("SmtpClient destroyed by on_error (greeting refused)", relay.port(),
                                "greeting: 554 no service / m");
    }
    if (want("error-message")) {
        Relay relay(Mode::refuse_greeting, id);
        test_destroyed_on_error("the message handed to on_error, read after it destroyed the client", relay.port(),
                                "greeting: 554", true);
    }
    if (want("hangup")) {
        Relay relay(Mode::hangup, id);
        test_destroyed_on_error("SmtpClient destroyed by on_error (relay closed)", relay.port(),
                                "connection closed unexpectedly");
    }
    if (want("refused")) {
        const ClosedPort closed;
        test_destroyed_on_error("SmtpClient destroyed by on_error (connection refused)", closed.port(), "connect failed");
    }
    if (want("done"))
        test_destroyed_on_done(id, false);
    if (want("message"))
        test_destroyed_on_done(id, true);
    if (want("retry"))
        test_retry_from_on_error(id);
    return ran;
}

} // namespace

int main(int argc, char** argv)
{
    ::signal(SIGPIPE, SIG_IGN);
    const std::string only = argc > 1 ? argv[1] : "";
    {
        Server probe([](Conn&, int) {});
        if (!probe.ok()) {
            std::printf("smtp_client_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    const Identity id("IP:127.0.0.1");
    const Trust    trust(id.pem());

    if (only.empty()) {
        test_envelope(id);
        test_all_refused(id);
        test_glued_220(id);
        test_endless_line(id);
        test_two_sends(id);
    }
    if (!test_destroyed_by_handler(id, only)) {
        std::printf("smtp_client_test: no case '%s'\n", only.c_str());
        return 2;
    }

    std::printf("smtp_client_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

#endif // WITH_SSL
