// http_split_test — HttpParser and HttpResponseParser read a message the same
// way however the bytes arrive: in one piece, in two pieces split at every
// position, and one byte at a time.
//
//   1. a request: every header keeps its whole name and its whole value (an
//      empty value included), in order; the handler is called once;
//   2. a response: the same, and Content-Length is read, so the body is whole.
//
// Usage: http_split_test

#include "apostol/http.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace apostol;

namespace
{

using Headers = std::vector<std::pair<std::string, std::string>>;

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string show(const Headers& h)
{
    std::string s;
    for (const auto& [n, v] : h)
        s += "[" + n + "]=[" + v + "] ";
    return s;
}

// The pieces of `msg`: cut at each position in `cuts` (ascending, inside msg).
std::vector<std::string> pieces(const std::string& msg, const std::vector<std::size_t>& cuts)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t c : cuts) {
        out.push_back(msg.substr(from, c - from));
        from = c;
    }
    out.push_back(msg.substr(from));
    return out;
}

const std::string kRequest =
    "POST /api/v1/x?a=1 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Authorization: Bearer T0K3N\r\n"
    "X-Empty:\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

const Headers kRequestHeaders = {
    {"Host", "example.test"},
    {"Authorization", "Bearer T0K3N"},
    {"X-Empty", ""},
    {"Content-Length", "5"},
};

const std::string kResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "X-Empty:\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "world";

const Headers kResponseHeaders = {
    {"Content-Type", "text/plain"},
    {"X-Empty", ""},
    {"Content-Length", "5"},
};

void request_case(const std::vector<std::string>& parts, const std::string& label)
{
    HttpParser parser;
    int calls = 0;
    HttpRequest got;
    parser.set_handler([&](HttpRequest r) { ++calls; got = std::move(r); });
    bool fed = true;
    for (const auto& p : parts)
        fed = parser.feed(p.data(), p.size()) && fed;
    check(fed, label + ": request feed failed");
    check(calls == 1, label + ": request handler calls " + std::to_string(calls));
    check(got.headers == kRequestHeaders, label + ": request headers " + show(got.headers));
    check(got.header("Authorization") == "Bearer T0K3N", label + ": Authorization lost");
    check(got.body == "hello", label + ": request body [" + got.body + "]");
}

void response_case(const std::vector<std::string>& parts, const std::string& label)
{
    HttpResponseParser parser;
    int calls = 0;
    HttpClientResponse got;
    parser.set_handler([&](HttpClientResponse r) { ++calls; got = std::move(r); });
    bool fed = true;
    for (const auto& p : parts)
        fed = parser.feed(p.data(), p.size()) && fed;
    check(fed, label + ": response feed failed");
    check(calls == 1, label + ": response handler calls " + std::to_string(calls));
    check(got.headers == kResponseHeaders, label + ": response headers " + show(got.headers));
    check(got.content_length() == 5, label + ": content_length " + std::to_string(got.content_length()));
    check(got.body == "world", label + ": response body [" + got.body + "]");
}

std::vector<std::size_t> every_byte(const std::string& msg)
{
    std::vector<std::size_t> cuts;
    for (std::size_t i = 1; i < msg.size(); ++i)
        cuts.push_back(i);
    return cuts;
}

} // namespace

int main()
{
    request_case(pieces(kRequest, {}), "request whole");
    for (std::size_t i = 1; i < kRequest.size(); ++i)
        request_case(pieces(kRequest, {i}), "request cut at " + std::to_string(i));
    request_case(pieces(kRequest, every_byte(kRequest)), "request byte by byte");

    response_case(pieces(kResponse, {}), "response whole");
    for (std::size_t i = 1; i < kResponse.size(); ++i)
        response_case(pieces(kResponse, {i}), "response cut at " + std::to_string(i));
    response_case(pieces(kResponse, every_byte(kResponse)), "response byte by byte");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("http_split: all checks passed\n");
    return 0;
}
