// http_framing_test — framing details of the HTTP server side and helpers:
//
//   1. a response carries one Content-Length, the size of what goes out, even
//      when the handler set the header itself (a HEAD answer keeps its own);
//   2. content_length() of a request and of a client response is 0 for a value
//      that is not a number or does not fit, never a wrapped number;
//   3. is_safe_path() refuses a ".." segment, not a name holding two dots;
//      a query string decodes as url_decode() does;
//   4. "Expect: 100-continue" on a request with a body is answered with
//      "100 Continue" before the body is read — after the answers already
//      due on the connection; not for HTTP/1.0, not without a body.
//
// Usage: http_framing_test

#include "apostol/file_utils.hpp"
#include "apostol/http.hpp"
#include "apostol/http_utils.hpp"
#include "apostol/tcp.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace apostol;

namespace
{

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::size_t count(const std::string& s, const std::string& what)
{
    std::size_t n = 0;
    for (auto at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
        ++n;
    return n;
}

// ── 1. One Content-Length ────────────────────────────────────────────────────

void content_length_once()
{
    HttpResponse r;
    r.set_status(HttpStatus::ok).set_body("hello", "text/plain");
    r.set_header("Content-Length", "99");
    const auto out = r.serialize();
    check(count(out, "Content-Length") == 1, "own Content-Length: header written " +
          std::to_string(count(out, "Content-Length")) + " times:\n" + out);
    check(out.find("Content-Length: 5\r\n") != std::string::npos, "own Content-Length: not the body size:\n" + out);

    HttpResponse lower;
    lower.set_status(HttpStatus::ok).set_body("hello", "text/plain");
    lower.set_header("content-length", "5");
    check(count(lower.serialize(), "ontent-") == 2, "lower-case content-length written twice:\n" + lower.serialize());

    HttpResponse head;
    head.set_status(HttpStatus::ok);
    head.set_header("Content-Length", "1234");
    head.suppress_body();
    const auto h = head.serialize();
    check(count(h, "Content-Length") == 1 && h.find("Content-Length: 1234\r\n") != std::string::npos,
          "HEAD: its own Content-Length is not kept once:\n" + h);
}

// ── 2. content_length() ──────────────────────────────────────────────────────

void content_length_value()
{
    auto req_with = [](std::string v) {
        HttpRequest r;
        r.headers.emplace_back("Content-Length", std::move(v));
        return r.content_length();
    };
    auto resp_with = [](std::string v) {
        HttpClientResponse r;
        r.headers.emplace_back("Content-Length", std::move(v));
        return r.content_length();
    };
    for (const char* v : {"18446744073709551617", "99999999999999999999999", "12a", "-1", " 5", ""}) {
        check(req_with(v) == 0, std::string("request content_length(\"") + v + "\") = " + std::to_string(req_with(v)));
        check(resp_with(v) == 0, std::string("response content_length(\"") + v + "\") = " + std::to_string(resp_with(v)));
    }
    check(req_with("18446744073709551615") == 18446744073709551615ULL, "request content_length: max value");
    check(req_with("42") == 42 && resp_with("42") == 42, "content_length(\"42\")");
}

// ── 3. Paths and decoding ────────────────────────────────────────────────────

void paths()
{
    for (const char* p : {"a..b.txt", "/files/v1..2/x", "..hidden", "name..", "/a/.../b"})
        check(is_safe_path(p), std::string("is_safe_path(\"") + p + "\") = false");
    for (const char* p : {"..", "../etc/passwd", "/a/../b", "/a/..", "a/..\\b", "..\\x", ""})
        check(!is_safe_path(p), std::string("is_safe_path(\"") + p + "\") = true");
    check(!is_safe_path(std::string_view("a\0b", 3)), "is_safe_path: NUL accepted");

    HttpParser parser;
    HttpRequest got;
    parser.set_handler([&](HttpRequest r) { got = std::move(r); });
    const std::string req = "GET /p?a=x%20y+z&b=%41%4&c=%zz HTTP/1.1\r\nHost: h\r\n\r\n";
    check(parser.feed(req.data(), req.size()), "query: not parsed");
    check(got.param("a") == url_decode("x%20y+z") && got.param("a") == "x y z", "query a: " + got.param("a"));
    check(got.param("b") == url_decode("%41%4"), "query b: " + got.param("b"));
    check(got.param("c") == url_decode("%zz"), "query c: " + got.param("c"));
}

// ── 4. Expect: 100-continue ──────────────────────────────────────────────────

struct Pair
{
    std::unique_ptr<HttpConnection> conn;
    int peer{-1};
    ~Pair() { if (peer >= 0) ::close(peer); }
};

void make_pair_conn(Pair& p)
{
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        std::exit(1);
    }
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    sockaddr_storage ss{};
    ss.ss_family = AF_UNIX;
    p.conn = std::make_unique<HttpConnection>(TcpConnection(sv[0], ss, sizeof(sa_family_t)), nullptr);
    p.peer = sv[1];
}

std::string drain(int fd)
{
    std::string out;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

void put(int fd, const std::string& s)
{
    if (::write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size())) {
        std::perror("write");
        std::exit(1);
    }
}

const std::string k_continue = "HTTP/1.1 100 Continue\r\n\r\n";

/// Sends @p head, reads what the server says before the body, then sends
/// @p body and reads the rest. Returns {before body, after body, handler calls}.
struct Exchange { std::string before, after; int calls{0}; std::string body; };

Exchange exchange(const std::string& head, const std::string& body)
{
    Pair p;
    make_pair_conn(p);
    Exchange ex;
    auto handler = [&](const HttpRequest& req, HttpResponse& resp) {
        ++ex.calls;
        ex.body = req.body;
        resp.set_status(HttpStatus::ok).set_body("done", "text/plain");
    };
    put(p.peer, head);
    p.conn->on_readable(handler);
    ex.before = drain(p.peer);
    if (!body.empty()) {
        put(p.peer, body);
        p.conn->on_readable(handler);
    }
    ex.after = drain(p.peer);
    return ex;
}

void expect_continue()
{
    {
        auto ex = exchange("POST /u HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n", "abc");
        check(ex.before == k_continue, "100-continue: before the body got \"" + ex.before + "\"");
        check(ex.calls == 1 && ex.body == "abc", "100-continue: handler calls " + std::to_string(ex.calls));
        check(ex.after.rfind("HTTP/1.1 200", 0) == 0 && count(ex.after, "100 Continue") == 0,
              "100-continue: final answer \"" + ex.after + "\"");
    }
    {
        auto ex = exchange("POST /u HTTP/1.1\r\nHost: h\r\nexpect: 100-Continue\r\nTransfer-Encoding: chunked\r\n\r\n",
                           "3\r\nabc\r\n0\r\n\r\n");
        check(ex.before == k_continue, "100-continue, chunked: before the body got \"" + ex.before + "\"");
        check(ex.calls == 1 && ex.body == "abc", "100-continue, chunked: body \"" + ex.body + "\"");
    }
    {
        auto ex = exchange("POST /u HTTP/1.0\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n", "abc");
        check(ex.before.empty(), "HTTP/1.0: interim answer sent: \"" + ex.before + "\"");
    }
    {
        auto ex = exchange("POST /u HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n", "");
        check(ex.before.rfind("HTTP/1.1 200", 0) == 0 && count(ex.before, "100 Continue") == 0,
              "no body: \"" + ex.before + "\"");
    }
    {
        auto ex = exchange("POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\n", "abc");
        check(ex.before.empty(), "no Expect: \"" + ex.before + "\"");
    }
    {
        // Pipelined: the answer to the first request goes before the interim one.
        auto ex = exchange("GET /a HTTP/1.1\r\nHost: h\r\n\r\n"
                           "POST /u HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n", "abc");
        const auto at_200 = ex.before.find("HTTP/1.1 200");
        const auto at_100 = ex.before.find(k_continue);
        check(at_200 == 0 && at_100 != std::string::npos && at_100 > at_200,
              "pipelined: order \"" + ex.before + "\"");
        check(ex.calls == 2, "pipelined: handler calls " + std::to_string(ex.calls));
    }
}

} // namespace

int main()
{
    content_length_once();
    content_length_value();
    paths();
    expect_continue();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("http_framing: all checks passed\n");
    return 0;
}
