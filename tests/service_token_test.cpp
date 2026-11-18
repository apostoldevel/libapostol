// service_token_test — ServiceToken's retry schedule, and how
// refresh_service_token() reads the answer of daemon.token.
//
//   1. the clock: std::chrono::steady_clock::now() is defined in this binary —
//      the real monotonic clock plus an offset the test moves — and a token
//      issued for 30 s is no longer valid 31 s later (the library reads it);
//   2. a token held: the retry delay doubles from 10 s, and no retry is
//      scheduled past the moment the token stops being valid;
//   3. no token held (it expired, or none was ever obtained): the delay stays
//      at most 60 s, so the first retry after the issuer is back comes within
//      a minute;
//   4. issued() clears the backoff;
//   5. the answer of daemon.token: a field of the wrong type, a body that is
//      not an object, a lifetime out of range — the call reports a failure and
//      nothing is thrown out of the pool's handler; a lifetime past the maximum
//      is clamped, not wrapped. A child process plays a server that answers
//      the query for daemon.token with the body of the case it names.
//
// Usage: service_token_test

#include "apostol/db_platform.hpp"
#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/pg.hpp"
#include "apostol/service_token.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <csignal>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace apostol;
using namespace std::chrono_literals;

namespace
{

std::chrono::nanoseconds g_offset{0};

} // namespace

// The clock every steady_clock::now() of this binary reads, the library's
// included: CLOCK_MONOTONIC (what libstdc++ reads) plus g_offset.
namespace std::chrono
{
steady_clock::time_point steady_clock::now() noexcept
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return time_point(duration(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec)))
        + g_offset;
}
} // namespace std::chrono

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("service_token_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("service_token_test: ok   %s\n", what.c_str());
    }
}

void advance(std::chrono::seconds s) { g_offset += s; }

// Seconds, in steps of one, until needs_refresh() says yes; -1 past @p limit.
long long until_refresh(ServiceToken& t, long long limit)
{
    for (long long s = 0; s <= limit; ++s) {
        if (t.needs_refresh())
            return s;
        advance(1s);
    }
    return -1;
}

// ─── 1. the clock ───────────────────────────────────────────────────────────

void clock_seen()
{
    ServiceToken t;
    t.issued("tok", "s", 30s);
    check(t.valid(), "clock: a token issued for 30 s is valid");
    advance(31s);
    check(!t.valid(), "clock: 31 s later it is not (the library reads this clock)");
}

// ─── 2, 3. a token held through an outage, then none ────────────────────────

void outage()
{
    ServiceToken t;
    t.issued("tok", "s", 3600s);
    const auto issued_at = std::chrono::steady_clock::now();
    const auto at = [&] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - issued_at).count();
    };

    check(until_refresh(t, 4000) == 3300, "outage: renewal starts 300 s before expiry");

    // The issuer is down from here on: every attempt fails.
    std::vector<long long> held_gaps;
    std::vector<long long> unheld_gaps;
    long long last_held_retry = -1;
    while (at() < 3600 + 2 * 3600) {
        t.begin_refresh();
        const bool held = t.valid();
        t.failed();
        const long long gap = until_refresh(t, 4000);
        if (gap < 0) {
            check(false, "outage: no retry within 4000 s");
            return;
        }
        (held ? held_gaps : unheld_gaps).push_back(gap);
        if (held)
            last_held_retry = at();
    }

    const std::vector<long long> first{10, 20, 40, 80};
    check(held_gaps.size() >= first.size() && std::equal(first.begin(), first.end(), held_gaps.begin()),
          "outage: while the token is held the delay doubles from 10 s");
    check(last_held_retry >= 3600 && last_held_retry <= 3601,
          "outage: the last retry of the held token comes as it expires (at "
              + std::to_string(last_held_retry) + " s)");

    long long worst = 0;
    for (auto g : unheld_gaps)
        worst = std::max(worst, g);
    check(!unheld_gaps.empty() && worst <= 60,
          "outage: with no valid token the delay is at most 60 s (worst " + std::to_string(worst) + " s)");

    // The issuer is back.
    t.begin_refresh();
    t.issued("tok2", "s2", 3600s);
    check(t.valid() && !t.needs_refresh(), "outage: issued() after the outage gives a valid token");
    advance(3300s);
    check(t.needs_refresh(), "outage: renewal of the new token");
    t.begin_refresh();
    t.failed();
    check(until_refresh(t, 100) == 10, "outage: issued() cleared the backoff (10 s again)");
}

void never_held()
{
    ServiceToken t;
    check(t.needs_refresh(), "never held: the first attempt is due at once");
    long long worst = 0;
    for (int i = 0; i < 20; ++i) {
        t.begin_refresh();
        t.failed();
        const long long gap = until_refresh(t, 4000);
        worst = std::max(worst, gap < 0 ? 4001 : gap);
    }
    check(worst <= 60, "never held: 20 failures, the delay is at most 60 s (worst " + std::to_string(worst) + " s)");
}

// The case of the first reading: a 30 s token, nine failures in a row, then
// the token expires — the next attempt must not wait out a half-hour backoff.
void short_token()
{
    ServiceToken t;
    t.issued("tok", "s", 30s);
    advance(15s);
    check(t.needs_refresh(), "short token: renewal at half its life");
    for (int i = 0; i < 9; ++i) {
        t.begin_refresh();
        t.failed();
    }
    advance(16s);
    check(!t.valid(), "short token: expired");
    check(t.needs_refresh(), "short token: a retry is due as soon as it expired");
}

// ─── 5. the answer of daemon.token ──────────────────────────────────────────

bool read_n(int fd, void* buf, std::size_t n)
{
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        const ssize_t r = ::read(fd, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

void write_all(int fd, const std::string& s)
{
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t w = ::write(fd, s.data() + off, s.size() - off);
        if (w <= 0)
            return;
        off += static_cast<std::size_t>(w);
    }
}

std::string be16(uint16_t v)
{
    v = htons(v);
    return std::string(reinterpret_cast<const char*>(&v), 2);
}

std::string be32(uint32_t v)
{
    v = htonl(v);
    return std::string(reinterpret_cast<const char*>(&v), 4);
}

std::string message(char type, const std::string& body)
{
    return std::string(1, type) + be32(static_cast<uint32_t>(body.size() + 4)) + body;
}

// One text column "token" holding @p value.
std::string one_row(const std::string& value)
{
    std::string desc = be16(1) + std::string("token\0", 6) + be32(0) + be16(0) + be32(25) + be16(0xFFFF)
        + be32(0xFFFFFFFF) + be16(0);
    std::string row = be16(1) + be32(static_cast<uint32_t>(value.size())) + value;
    return message('T', desc) + message('D', row) + message('C', std::string("SELECT 1\0", 9)) + message('Z', "I");
}

// The child: connections one after another until the parent stops it; the
// query for daemon.token is answered with the body of the case its client id
// names ("case<N>"), anything else as done with no rows. Keyed by the query,
// not by the order of connections: a pool reconnects after a handler fails.
[[noreturn]] void serve(int listen_fd, const std::vector<std::string>& bodies)
{
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0)
            _exit(2);
        uint32_t len = 0;
        if (!read_n(fd, &len, 4)) {
            ::close(fd);
            continue;
        }
        std::string startup(ntohl(len) - 4, '\0');
        if (!read_n(fd, startup.data(), startup.size())) {
            ::close(fd);
            continue;
        }
        write_all(fd, message('R', std::string("\0\0\0\0", 4)) + message('Z', "I"));

        for (;;) {
            char type = 0;
            if (!read_n(fd, &type, 1) || !read_n(fd, &len, 4))
                break;
            std::string m(ntohl(len) - 4, '\0');
            if (!read_n(fd, m.data(), m.size()) || type == 'X')
                break;
            if (type != 'Q')
                continue;
            const auto at = m.find("'case");
            if (m.find("daemon.token(") != std::string::npos && at != std::string::npos) {
                const std::size_t n = std::stoul(m.substr(at + 5));
                write_all(fd, one_row(n < bodies.size() ? bodies[n] : std::string("{}")));
            } else {
                write_all(fd, message('C', std::string("SELECT 0\0", 9)) + message('Z', "I"));
            }
        }
        ::close(fd);
    }
}

// What the loop says about a handler that threw.
struct Capture : LogTarget
{
    std::string* out;
    explicit Capture(std::string* o) : out(o) {}
    void write(LogLevel, std::string_view m) override { out->append(m).append("\n"); }
};

struct Answer
{
    const char* name;
    std::string body;
    bool        valid;   // a usable token expected
};

void answers()
{
    const std::vector<Answer> cases{
        {"control",                     R"({"access_token":"tok","session":"s","expires_in":3600})", true},
        {"access_token a number",       R"({"access_token":123,"expires_in":3600})",                 false},
        {"session a number",            R"({"access_token":"tok","session":7,"expires_in":3600})",   false},
        {"the body an array",           R"([1,2])",                                                  false},
        {"the body a string",           R"("tok")",                                                  false},
        {"error members not strings",   R"({"error":{"error":5,"message":[1]}})",                    false},
        {"expires_in 1e300",            R"({"access_token":"tok","session":"s","expires_in":1e300})", true},
        {"expires_in -1e300",           R"({"access_token":"tok","session":"s","expires_in":-1e300})", false},
        {"expires_in 59.9",             R"({"access_token":"tok","session":"s","expires_in":59.9})",  true},
    };

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 4) != 0
        || ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen) != 0) {
        check(false, "answers: a listening socket");
        return;
    }

    std::vector<std::string> bodies;
    for (const auto& c : cases)
        bodies.push_back(c.body);

    const pid_t child = ::fork();
    if (child == 0)
        serve(lfd, bodies);
    ::close(lfd);

    const std::string conninfo = "host=127.0.0.1 port=" + std::to_string(ntohs(a.sin_port))
        + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=5";

    for (std::size_t n = 0; n < cases.size(); ++n) {
        const auto&  c = cases[n];
        EventLoop    loop;
        Logger       log;
        ServiceToken token;
        std::string  thrown;
        std::string  said;
        Logger       diag;
        diag.add_target(std::make_unique<Capture>(&said));
        EventLoop::set_diagnostics(&diag);
        {
            PgPool pool{loop, conninfo, 1, 1};
            pool.set_reset({});
            pool.start();
            db_platform::refresh_service_token(pool, token, log, "test", "case" + std::to_string(n), "secret", "", "agent", "host");
            try {
                for (int i = 0; i < 40 && token.refreshing(); ++i)
                    loop.run_for(50ms);
            } catch (const std::exception& e) {
                thrown = e.what();
            } catch (...) {
                thrown = "(not std::exception)";
            }
        }
        EventLoop::unset_diagnostics(&diag);

        const std::string name = std::string("answers: ") + c.name;
        check(thrown.empty() && said.empty(),
              name + ": no handler threw" + (thrown.empty() && said.empty() ? "" : " (" + thrown + said + ")"));
        check(!token.refreshing(), name + ": the outcome was reported");
        check(token.valid() == c.valid, name + (c.valid ? ": a valid token" : ": no token, a failure"));
        if (c.valid) {
            check(token.token() == "tok" && token.session() == "s", name + ": token and session as sent");
        }
        if (std::string_view(c.name) == "expires_in 1e300") {
            advance(ServiceToken::k_max_life - 1s);
            check(token.valid(), name + ": clamped to the maximum life — valid just before it");
            advance(2s);
            check(!token.valid(), name + ": and not after it");
        }
        if (std::string_view(c.name) == "expires_in 59.9") {
            advance(59s);
            check(!token.valid(), name + ": the life is the whole seconds, 59");
        }
    }

    ::kill(child, SIGTERM);
    ::waitpid(child, nullptr, 0);
}

} // namespace

int main()
{
    clock_seen();
    short_token();
    outage();
    never_held();
    answers();

    if (g_failed) {
        std::printf("service_token_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("service_token_test: all passed\n");
    return 0;
}
