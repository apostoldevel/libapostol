// http_framing_test — framing details of the HTTP server side and helpers:
//
//   1. a response carries one Content-Length, the size of what goes out, even
//      when the handler set the header itself (a HEAD answer keeps its own);
//   2. content_length() of a request and of a client response is 0 for a value
//      that is not a number or does not fit, never a wrapped number;
//   3. is_safe_path() refuses a ".." segment, not a name holding two dots;
//      a query string decodes as url_decode() does;
//   3a. trailers of a chunked message are not taken for headers, by either
//      parser;
//   5. a FIFO is refused as a file body without waiting; one file response
//      sent twice behind a file in flight goes out whole twice;
//   6. a request whose "100 Continue" could not be written is not run;
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
#include <sys/stat.h>
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

// ── 3a. Trailers stay out of the headers ─────────────────────────────────────

void trailers()
{
    {
        HttpParser parser;
        HttpRequest got;
        int calls = 0;
        parser.set_handler([&](HttpRequest r) { ++calls; got = std::move(r); });
        const std::string req = "POST /t HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "3\r\nabc\r\n0\r\nX-Trailer-One: 1\r\nX-Trailer-Two: 2\r\n\r\n"
                                "GET /n HTTP/1.1\r\nHost: h\r\nX-Next: 3\r\n\r\n";
        check(parser.feed(req.data(), req.size()), "request trailers: not parsed");
        check(calls == 2, "request trailers: handler calls " + std::to_string(calls));
        check(got.header("X-Next") == "3" && got.header("X-Trailer-One").empty(), "request after trailers: headers wrong");
    }
    {
        HttpParser parser;
        HttpRequest first;
        int calls = 0;
        parser.set_handler([&](HttpRequest r) { if (++calls == 1) first = std::move(r); });
        const std::string req = "POST /t HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "3\r\nabc\r\n0\r\nX-Trailer-One: 1\r\nX-Trailer-Two: 2\r\n\r\n";
        check(parser.feed(req.data(), req.size()), "request trailers: not parsed");
        check(first.body == "abc", "request trailers: body \"" + first.body + "\"");
        check(first.header("X-Trailer-One").empty() && first.header("X-Trailer-Two").empty(),
              "request trailers: a trailer became a header");
        check(first.headers.size() == 2, "request trailers: " + std::to_string(first.headers.size()) + " headers");
    }
    {
        HttpResponseParser parser;
        HttpClientResponse got;
        parser.set_handler([&](HttpClientResponse r) { got = std::move(r); });
        const std::string resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                 "2\r\nok\r\n0\r\nX-Trailer-One: 1\r\nX-Trailer-Two: 2\r\n\r\n";
        check(parser.feed(resp.data(), resp.size()), "response trailers: not parsed");
        check(got.body == "ok", "response trailers: body \"" + got.body + "\"");
        check(got.header("X-Trailer-One").empty() && got.header("X-Trailer-Two").empty(),
              "response trailers: a trailer became a header");
        check(got.headers.size() == 1, "response trailers: " + std::to_string(got.headers.size()) + " headers");
    }
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


// ── 5. Files ─────────────────────────────────────────────────────────────────

std::string temp_file(std::size_t size, char c)
{
    char tmpl[] = "/tmp/http_framing_test.XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { std::perror("mkstemp"); std::exit(1); }
    const std::string data(size, c);
    if (::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) { std::perror("write"); std::exit(1); }
    ::close(fd);
    return tmpl;
}

void fifo()
{
    // A FIFO is refused at once, not waited on for a writer (alarm: a
    // blocking open() ends the test here — it runs last).
    char dir[] = "/tmp/http_framing_test.d.XXXXXX";
    if (!::mkdtemp(dir)) { std::perror("mkdtemp"); std::exit(1); }
    const std::string fifo = std::string(dir) + "/fifo";
    if (::mkfifo(fifo.c_str(), 0600) != 0) { std::perror("mkfifo"); std::exit(1); }
    ::alarm(5);
    HttpResponse r;
    check(!r.set_file(fifo, "text/plain"), "set_file: a FIFO accepted");
    {
        Pair p;
        make_pair_conn(p);
        p.conn->send_file(fifo, "text/plain");
        check(drain(p.peer).rfind("HTTP/1.1 404", 0) == 0, "send_file: a FIFO not refused");
    }
    ::alarm(0);
    ::unlink(fifo.c_str());
    ::rmdir(dir);
}

void file_twice()
{
    // One file response sent twice behind a file in flight: both go out whole.
    const auto big = temp_file(4 * 1024 * 1024, 'x');
    const auto small = temp_file(10, 'y');
    Pair p;
    make_pair_conn(p);
    int tiny = 4096;
    ::setsockopt(p.peer, SOL_SOCKET, SO_RCVBUF, &tiny, sizeof tiny);
    p.conn->send_file(big, "application/octet-stream");
    HttpResponse twice;
    twice.set_status(HttpStatus::ok);
    check(twice.set_file(small, "text/plain"), "set_file: a regular file refused");
    p.conn->send_response(twice);
    p.conn->send_response(twice);
    std::string got;
    for (int i = 0; i < 100000; ++i) {
        got += drain(p.peer);
        if (!p.conn->on_writable())
            break;
    }
    got += drain(p.peer);
    check(count(got, std::string(10, 'y')) == 2, "file twice: " + std::to_string(count(got, "yyyyyyyyyy")) +
          " whole bodies of 2");
    check(!p.conn->closed(), "file twice: connection closed");
    ::unlink(big.c_str());
    ::unlink(small.c_str());
}

void continue_to_a_gone_client()
{
    // The client's side is closed for reading: "100 Continue" cannot go out,
    // and the request whose body is already here is not run.
    Pair p;
    make_pair_conn(p);
    put(p.peer, "POST /u HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc");
    ::shutdown(p.peer, SHUT_RD);
    int calls = 0;
    p.conn->on_readable([&](const HttpRequest&, HttpResponse& resp) {
        ++calls;
        resp.set_status(HttpStatus::ok).set_body("done", "text/plain");
    });
    check(calls == 0, "gone client: the request was run");
}
} // namespace

int main()
{
    content_length_once();
    content_length_value();
    paths();
    trailers();
    expect_continue();
    file_twice();
    continue_to_a_gone_client();
    fifo();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("http_framing: all checks passed\n");
    return 0;
}
