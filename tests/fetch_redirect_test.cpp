// fetch_redirect_test — how CurlClient and FetchClient (curl backend) treat an
// HTTP redirect, in this process, against a server of its own on 127.0.0.1.
//
// The server answers /r<code> with that redirect status and Location: /target,
// /loop with 302 and Location: /loop; /target answers 200 with
// "<method> <body length>" and counts its hits.
//
// For each of 301, 302, 303, 307, 308:
//   1. a GET or HEAD-family request that sends no body follows the redirect:
//      status 200, /target hit once (GET, "get", and GET given content, which
//      a GET does not send);
//   2. every request that sends a body (POST, PUT, PATCH, DELETE, "post", and
//      "get" with content, which goes out as a custom request with a body), and
//      a DELETE without one, does not follow it: the caller gets the redirect
//      status itself with its Location header, and /target is never reached
//      (neither as the same method without its body, nor as a GET).
// And a GET into a redirect loop fails after at most 5 redirects.
//
// Usage: fetch_redirect_test (exit 77 — skipped — without a loopback listener)

#include "net_test_util.hpp"

#include "apostol/fetch_client.hpp"
#ifdef WITH_CURL
#include "apostol/curl_client.hpp"
#endif

#include <signal.h>

#include <atomic>
#include <memory>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
        std::printf("fetch_redirect_test: FAIL %s\n", what.c_str());
    }
}

const std::string kBody = R"({"idTag":"0123456789","connectorId":1,"x":1})";

std::string lower(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

class Redirector
{
public:
    Redirector()
        : server_([this](Conn& c, int) { serve(c); })
    {}

    bool        ok() const { return server_.ok(); }
    std::string url(const std::string& path) const
    {
        return "http://127.0.0.1:" + std::to_string(server_.port()) + path;
    }
    int         target_hits() const { return hits_.load(); }
    int         loop_hits() const { return loops_.load(); }
    std::string last_target() const
    {
        std::lock_guard<std::mutex> g(mu_);
        return last_;
    }

private:
    void serve(Conn& c)
    {
        std::string req;
        if (!read_until(c, req, "\r\n\r\n"))
            return;
        const auto head_end = req.find("\r\n\r\n") + 4;
        const auto sp1      = req.find(' ');
        const auto sp2      = req.find(' ', sp1 + 1);
        const std::string method = req.substr(0, sp1);
        const std::string path   = req.substr(sp1 + 1, sp2 - sp1 - 1);

        // Content-Length, by a case-insensitive search of the head only.
        std::size_t length = 0;
        const std::string head = lower(req.substr(0, head_end));
        const auto p = head.find("\r\ncontent-length:");
        if (p != std::string::npos)
            length = std::strtoul(head.c_str() + p + std::strlen("\r\ncontent-length:"), nullptr, 10);
        while (req.size() - head_end < length && c.read(req, 3000) == Rd::data) {
        }
        const std::size_t got = req.size() - head_end;

        if (path == "/loop") {
            ++loops_;
            c.write("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if (path.rfind("/r", 0) == 0) {
            const std::string code = path.substr(2);
            c.write("HTTP/1.1 " + code + " Moved\r\nLocation: /target\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
        } else {
            const std::string answer = method + " " + std::to_string(got);
            {
                std::lock_guard<std::mutex> g(mu_);
                last_ = answer;
            }
            ++hits_;
            c.write("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(answer.size())
                    + "\r\nConnection: close\r\n\r\n" + answer);
        }
        c.close_now();
    }

    std::atomic<int>   hits_{0};
    std::atomic<int>   loops_{0};
    mutable std::mutex mu_;
    std::string        last_;
    Server             server_;
};

struct Result
{
    bool        responded{false};
    int         status{0};
    std::string body;
    std::string location;
    bool        failed{false};
    std::string error;
    bool        finished() const { return responded || failed; }
};

template <class Headers>
std::string find_location(const Headers& headers)
{
    for (const auto& [k, v] : headers)
        if (lower(k) == "location")
            return v;
    return {};
}

struct Case
{
    const char* method;
    bool        with_body;
    bool        follows;
    const char* arrives;   // what /target reports when followed
};

const Case kCases[] = {
    {"GET", false, true, "GET 0"},
    {"get", false, true, "get 0"},
    {"GET", true, true, "GET 0"},
    {"get", true, false, ""},
    {"POST", true, false, ""},
    {"post", true, false, ""},
    {"PUT", true, false, ""},
    {"PATCH", true, false, ""},
    {"DELETE", true, false, ""},
    {"DELETE", false, false, ""},
};

std::string name_of(const Case& k)
{
    return std::string(k.method) + (k.with_body ? " with a body" : " without a body");
}

void judge(const std::string& who, const Case& k, int code, const Result& r, const Redirector& srv)
{
    const std::string label = who + ", " + name_of(k) + " via " + std::to_string(code);
    if (k.follows) {
        check(r.responded && r.status == 200 && r.body == k.arrives && srv.target_hits() == 1,
              label + ": followed to the target, got status " + std::to_string(r.status) + ", body '" + r.body
                  + "', target hits " + std::to_string(srv.target_hits()) + ", error '" + r.error + "'");
        return;
    }
    check(r.responded && r.status == code && r.location == "/target" && srv.target_hits() == 0,
          label + ": the redirect returned to the caller, got status " + std::to_string(r.status) + ", Location '"
              + r.location + "', target hits " + std::to_string(srv.target_hits()) + " (last '"
              + srv.last_target() + "'), error '" + r.error + "'");
}

#ifdef WITH_CURL
template <class Send>
void drive(const std::string& who, const Case& k, int code, Send send)
{
    Redirector srv;
    EventLoop  loop;
    Result     r;
    // The client is held here, after the loop, so it goes before the loop does.
    std::shared_ptr<void> client = send(loop, srv.url("/r" + std::to_string(code)), k.with_body ? kBody : std::string(), r);
    run_until(loop, [&] { return r.finished(); }, 6000);
    // A client that wrongly follows reaches /target soon after the 3xx; wait
    // for that a little before judging a case that must not follow.
    if (!k.follows)
        run_until(loop, [&] { return srv.target_hits() > 0; }, 300);
    settle(loop, 50);
    judge(who, k, code, r, srv);
}

void with_curl(const Case& k, int code)
{
    drive("CurlClient", k, code, [&](EventLoop& loop, const std::string& url, const std::string& body, Result& r) {
        auto c = std::make_shared<CurlClient>(loop);
        c->perform(url, k.method, body, {{"Content-Type", "application/json"}},
                   [&r](CurlResponse resp) {
                       r.responded = true;
                       r.status    = resp.status_code;
                       r.body      = resp.body;
                       r.location  = find_location(resp.headers);
                   },
                   [&r](int, std::string_view e) { r.failed = true; r.error = std::string(e); });
        return std::shared_ptr<void>(c);
    });
}

void with_fetch(const Case& k, int code)
{
    drive("FetchClient(curl)", k, code, [&](EventLoop& loop, const std::string& url, const std::string& body, Result& r) {
        auto c = std::make_shared<FetchClient>(loop, FetchClient::Backend::curl);
        c->request(k.method, url, body, {{"Content-Type", "application/json"}},
                   [&r](FetchResponse resp) {
                       r.responded = true;
                       r.status    = resp.status_code;
                       r.body      = resp.body;
                       r.location  = find_location(resp.headers);
                   },
                   [&r](std::string_view e) { r.failed = true; r.error = std::string(e); });
        return std::shared_ptr<void>(c);
    });
}

void redirect_loop()
{
    Redirector srv;
    EventLoop  loop;
    Result     r;
    CurlClient c(loop);
    c.get(srv.url("/loop"), {},
          [&](CurlResponse resp) { r.responded = true; r.status = resp.status_code; },
          [&](int, std::string_view e) { r.failed = true; r.error = std::string(e); });
    run_until(loop, [&] { return r.finished(); }, 6000);
    settle(loop, 50);
    check(r.failed && !r.responded && srv.loop_hits() == 6,
          "CurlClient, GET into a redirect loop: fails after 5 redirects, got " + std::string(r.failed ? "failure '" + r.error + "'" : "status " + std::to_string(r.status))
              + ", requests " + std::to_string(srv.loop_hits()));
}
#endif

} // namespace

int main()
{
#ifndef WITH_CURL
    std::printf("fetch_redirect_test: SKIP built without libcurl\n");
    return 77;
#else
    ::signal(SIGPIPE, SIG_IGN);
    {
        Redirector probe;
        if (!probe.ok()) {
            std::printf("fetch_redirect_test: SKIP no listener on 127.0.0.1\n");
            return 77;
        }
    }

    for (const auto& k : kCases) {
        for (int code : {301, 302, 303, 307, 308}) {
            with_curl(k, code);
            with_fetch(k, code);
        }
    }
    redirect_loop();

    std::printf("fetch_redirect_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
#endif
}
