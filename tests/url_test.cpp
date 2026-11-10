// url_test — how HttpClient and WsClient read a URL, in this process, against
// listeners of their own on 127.0.0.1 and [::1] (no server: only which port is
// dialled and the first bytes the client sends there).
//
//   1. HttpClient takes a port of digits 1..65535 only; any other port, a host
//      with userinfo, an IPv6 address outside brackets or not an address, a
//      malformed scheme, a blank or a control byte is refused as "invalid URL"
//      and nothing is dialled. Every invalid form built on 127.0.0.1 uses the
//      listener's port, so a form read as that port is seen; the IPv6-shaped
//      ones can only be told by their error;
//   2. a query or a fragment right after the authority is not a part of the
//      host; a "://" inside a path or a query is not a scheme;
//   3. a scheme other than http/https is refused by its name first, whatever
//      the rest of the URL;
//   4. the Host header carries a port that is not the scheme's default;
//   5. the scheme is read in any case: "HTTPS://" and "WSS://" open TLS (the
//      listener sees a TLS record, not "GET");
//   6. WsClient: the same rules; a scheme other than ws/wss is refused by name;
//   7. --ipv6: an IPv6 address in brackets is dialled, and the Host header
//      writes it in brackets with the port.
//
// Usage: url_test [--ipv6]   (exit 77 — skipped — without a loopback listener;
//                             with --ipv6, also when [::1] is not reachable)

#include "apostol/event_loop.hpp"
#include "apostol/http_client.hpp"
#include "apostol/ws_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
        std::printf("url_test: FAIL %s\n", what.c_str());
    }
}

// A non-blocking listener on the loopback of one family, an ephemeral port.
struct Listener
{
    int           fd{-1};
    std::uint16_t port{0};

    explicit Listener(int family)
    {
        fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return;
        sockaddr_storage ss{};
        socklen_t        len = 0;
        if (family == AF_INET6) {
            auto* a        = reinterpret_cast<sockaddr_in6*>(&ss);
            a->sin6_family = AF_INET6;
            a->sin6_addr   = in6addr_loopback;
            len            = sizeof *a;
        } else {
            auto* a            = reinterpret_cast<sockaddr_in*>(&ss);
            a->sin_family      = AF_INET;
            a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            len                = sizeof *a;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0 || ::listen(fd, 64) != 0
            || ::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
            return;
        port = ntohs(family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                        : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    }
    ~Listener()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    // The connections queued since the last call; the bytes the first one sent.
    int drain(std::string* first = nullptr) const
    {
        int n = 0;
        for (int c; (c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC)) >= 0; ++n) {
            if (n == 0 && first) {
                char buf[2048];
                const auto got = ::recv(c, buf, sizeof buf, MSG_DONTWAIT);
                first->assign(buf, got > 0 ? static_cast<std::size_t>(got) : 0);
            }
            ::close(c);
        }
        return n;
    }
};

// Whether a plain connect to the listener completes: some hosts filter [::1].
bool reachable(const Listener& l)
{
    const int c = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c < 0)
        return false;
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr   = in6addr_loopback;
    a.sin6_port   = htons(l.port);
    bool ok = ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    if (!ok && errno == EINPROGRESS) {
        pollfd pfd{c, POLLOUT, 0};
        int    err = 0;
        socklen_t len = sizeof err;
        ok = ::poll(&pfd, 1, 500) == 1 && ::getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
    }
    ::close(c);
    l.drain();
    return ok;
}

void run_for(EventLoop& loop, std::chrono::milliseconds ms)
{
    loop.add_timer(ms, [&loop] { loop.stop(); }, false);
    loop.run();
}

struct Outcome
{
    std::string error;   // the first error reported, empty if none
    bool        dialled{false};
    std::string sent;    // the first bytes the listener received
};

Outcome http_attempt(const Listener& l, const std::string& url)
{
    EventLoop loop;
    Outcome   out;
    {
        HttpClient client(loop);
        client.get(url, {}, [](HttpClientResponse) {},
                   [&out](std::string_view e) {
                       if (out.error.empty())
                           out.error = std::string(e);
                   });
        run_for(loop, std::chrono::milliseconds(100));
    }
    out.dialled = l.drain(&out.sent) > 0;
    return out;
}

Outcome ws_attempt(const Listener& l, const std::string& url)
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
    out.dialled = l.drain(&out.sent) > 0;
    return out;
}

std::string first_line(const std::string& s)
{
    return s.substr(0, s.find("\r\n"));
}

bool has_header(const std::string& sent, const std::string& line)
{
    return sent.find("\r\n" + line + "\r\n") != std::string::npos;
}

std::string shown(const std::string& url, const Outcome& o)
{
    return "'" + url + "': error '" + o.error + "', dialled " + (o.dialled ? "yes" : "no") + ", sent '"
         + first_line(o.sent) + "'";
}

} // namespace

int ipv6_cases()
{
    const Listener l6(AF_INET6);
    if (l6.port == 0 || !reachable(l6)) {
        std::printf("url_test: SKIP [::1] not reachable here\n");
        return 77;
    }
    const std::string p6 = std::to_string(l6.port);
    {
        const auto url = "http://[::1]:" + p6 + "/v6";
        const auto o   = http_attempt(l6, url);
        check(o.error.empty() && o.dialled && first_line(o.sent) == "GET /v6 HTTP/1.1"
                  && has_header(o.sent, "Host: [::1]:" + p6),
              "http ipv6 " + shown(url, o));
    }
    {
        const auto url = "ws://[0:0::1]:" + p6 + "/v6";
        const auto o   = ws_attempt(l6, url);
        check(o.error.empty() && o.dialled && first_line(o.sent) == "GET /v6 HTTP/1.1"
                  && has_header(o.sent, "Host: [0:0::1]:" + p6),
              "ws ipv6 " + shown(url, o));
    }
    for (const auto& url : {"http://[::1]x:" + p6 + "/", "http://[::1]:" + p6 + "x/", "http://[]:" + p6 + "/",
                            "http://[:::]:" + p6 + "/", "http://[1:2]:" + p6 + "/", "http://[::1%25lo]:" + p6 + "/",
                            "http://[::1]:" + p6 + ":" + p6 + "/"}) {
        const auto o = http_attempt(l6, url);
        check(o.error == "invalid URL" && !o.dialled, "http ipv6 " + shown(url, o));
    }

    std::printf("url_test --ipv6: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--ipv6") == 0)
        return ipv6_cases();

    const Listener l4(AF_INET);
    if (l4.port == 0) {
        std::printf("url_test: SKIP no listener on 127.0.0.1\n");
        return 77;
    }
    const std::string p = std::to_string(l4.port);
    const std::string h = "http://127.0.0.1:";

    // ── HttpClient: taken ────────────────────────────────────────────────────
    for (const auto& url : {h + p + "/", h + p, h + "00" + p + "/", h + "0" + p + "/a?b=1", "127.0.0.1:" + p + "/",
                            h + p + "/a@b:c/%41%20", h + p + "/\xc3\xa9"}) {
        const auto o = http_attempt(l4, url);
        check(o.error.empty() && o.dialled, "http " + shown(url, o));
    }
    const std::vector<std::pair<std::string, std::string>> lines = {
        {h + p + "?q=a:b", "GET /?q=a:b HTTP/1.1"},
        {h + p + "/x#frag", "GET /x HTTP/1.1"},
        {h + p + "#x/y", "GET / HTTP/1.1"},
        {h + p + "/x#a b", "GET /x HTTP/1.1"},
        {"HTTP://127.0.0.1:" + p + "/", "GET / HTTP/1.1"},
        {"127.0.0.1:" + p + "/cb?u=http://other/", "GET /cb?u=http://other/ HTTP/1.1"},
    };
    for (const auto& [url, line] : lines) {
        const auto o = http_attempt(l4, url);
        check(o.error.empty() && o.dialled && first_line(o.sent) == line, "http request line " + shown(url, o));
    }
    {
        const auto url = h + p + "/";
        const auto o   = http_attempt(l4, url);
        check(o.dialled && has_header(o.sent, "Host: 127.0.0.1:" + p), "http Host with port " + shown(url, o));
    }

    // ── HttpClient: refused, nothing dialled ─────────────────────────────────
    const std::vector<std::string> bad = {
        h + "/", h + "0/", h + "65536/", h + "-1/", h + "99999999999999999999/",
        h + std::to_string(l4.port + 65536) + "/",
        h + p + "x/", h + p.substr(0, 1) + "x" + p.substr(1) + "/", h + p + " /", h + " " + p + "/",
        h + "+" + p + "/", h + p + ":" + p + "/",
        "http://:" + p + "/", "http://u:p@127.0.0.1:" + p + "/", "http://u@127.0.0.1:" + p + "/",
        "http://[127.0.0.1]:" + p + "/", "http://127.0.0.1]:" + p + "/",
        h + p + "/a b", h + p + "/a\r\nb", h + p + "/a?b\x7f",
        "1http://127.0.0.1:" + p + "/", "ht tp://127.0.0.1:" + p + "/", "ht\r\ntp://127.0.0.1:" + p + "/",
        "://127.0.0.1:" + p + "/",
        // IPv6-shaped: refused by the parser; told by the error only.
        "http://::1:" + p + "/", "http://[::1:" + p + "/",
    };
    for (const auto& url : bad) {
        const auto o = http_attempt(l4, url);
        check(o.error == "invalid URL" && !o.dialled, "http " + shown(url, o));
    }
    for (const auto& url : {"gopher://127.0.0.1:" + p + "/", "GOPHER://127.0.0.1:" + p + "/", std::string("gopher://u@h h:0/")}) {
        const auto o = http_attempt(l4, url);
        check(o.error == "unsupported URL scheme 'gopher'" && !o.dialled, "http scheme " + shown(url, o));
    }
#ifdef WITH_SSL
    {
        const auto url = "HTTPS://127.0.0.1:" + p + "/";
        const auto o   = http_attempt(l4, url);
        check(o.dialled && !o.sent.empty() && static_cast<unsigned char>(o.sent[0]) == 0x16,
              "https scheme case " + shown(url, o));
    }
#endif

    // ── WsClient ─────────────────────────────────────────────────────────────
    const std::string w = "ws://127.0.0.1:";
    for (const auto& url : {"ws://u@127.0.0.1:" + p + "/", "ws://u:p@127.0.0.1:" + p + "/", "ws://::1:" + p + "/",
                            "ws://[127.0.0.1]:" + p + "/", w + p + "/a b", w + p + "/a\r\nb", "127.0.0.1:" + p + "/",
                            "w s://127.0.0.1:" + p + "/"}) {
        const auto o = ws_attempt(l4, url);
        check(o.error == "invalid URL" && !o.dialled, "ws " + shown(url, o));
    }
    for (const auto& [url, scheme] : std::vector<std::pair<std::string, std::string>>{
             {"http://127.0.0.1:" + p + "/", "http"}, {"HTTPS://127.0.0.1:" + p + "/", "https"},
             {"ftp://u@h h:0/", "ftp"}}) {
        const auto o = ws_attempt(l4, url);
        check(o.error == "unsupported URL scheme '" + scheme + "'" && !o.dialled, "ws scheme " + shown(url, o));
    }
    {
        const auto url = "WS://127.0.0.1:" + p + "/x?y#z";
        const auto o   = ws_attempt(l4, url);
        check(o.dialled && first_line(o.sent) == "GET /x?y HTTP/1.1" && has_header(o.sent, "Host: 127.0.0.1:" + p),
              "ws scheme case " + shown(url, o));
    }
#ifdef WITH_SSL
    {
        // "WSS" is wss: the first byte on the wire is a TLS handshake record.
        const auto url = "WSS://127.0.0.1:" + p + "/";
        const auto o   = ws_attempt(l4, url);
        check(o.dialled && !o.sent.empty() && static_cast<unsigned char>(o.sent[0]) == 0x16,
              "wss scheme case " + shown(url, o));
    }
#endif

    std::printf("url_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
