// pg_queue_test — the bound and the age of PgPool's queue
// (set_queue_limit(), set_queue_timeout()).
//
//   1. limit: against a server that does not exist (127.0.0.1, port 1), the
//      queries past the limit are refused with queue_full_refusal — from the
//      loop, never from inside execute() — and the ones within it wait;
//   2. no limit after detach(): what is executed since is queued;
//   3. timeout: against the same server, every queued query fails with
//      "not sent: waited longer than …" once past the limit, and the queue is
//      empty;
//   4. nothing stale reaches the server: a server (a child process speaking
//      just enough of the protocol) that finishes the handshake only after the
//      limit. The queries asked before are failed — by the watch, or as the
//      connection takes them — and never received; one asked just before the
//      handshake ends is received and answered;
//   5. control — no limit, no timeout: the queries wait, none is failed.
//
// Usage: pg_queue_test

#include "apostol/event_loop.hpp"
#include "apostol/pg.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace apostol;
using namespace std::chrono_literals;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("pg_queue_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("pg_queue_test: ok   %s\n", what.c_str());
    }
}

const char* kNowhere = "host=127.0.0.1 port=1 connect_timeout=1";

void limit()
{
    EventLoop loop;
    PgPool pool{loop, kNowhere, 1, 1};
    pool.set_queue_limit(3);
    pool.start();

    int refused = 0, other = 0;
    for (int i = 0; i < 5; ++i)
        pool.execute("SELECT 1", {}, [&](std::string_view e) {
            (e == PgPool::queue_full_refusal ? refused : other)++;
        });
    check(refused == 0, "limit: not refused from inside execute()");
    check(pool.queue_size() == 3, "limit: three wait (queue_size " + std::to_string(pool.queue_size()) + ")");
    loop.run_for(50ms);
    check(refused == 2, "limit: the two past it refused (" + std::to_string(refused) + ")");
    check(pool.queue_size() == 3, "limit: the three still wait");

    pool.detach();
    pool.execute("SELECT 2", {}, [&](std::string_view) { ++other; });
    check(pool.queue_size() == 4, "after detach(): queued past the limit");
}

void timeout()
{
    EventLoop loop;
    PgPool pool{loop, kNowhere, 1, 1};
    pool.set_queue_timeout(1s);
    pool.start();

    int expired = 0, other = 0;
    bool result_not_ok = false;
    for (int i = 0; i < 100; ++i)
        pool.execute("SELECT 1", {}, [&](std::string_view e) {
            (std::string_view(e).starts_with("not sent: waited longer than 1s") ? expired : other)++;
        });
    // A caller with no error handler gets a result that is not ok().
    pool.execute("SELECT 1", [&](std::vector<PgResult> r) {
        result_not_ok = r.size() == 1 && !r[0].ok();
    });
    loop.run_for(500ms);
    check(expired == 0, "timeout: nothing failed before the limit");
    loop.run_for(2500ms);
    check(expired == 100, "timeout: every query failed past it (" + std::to_string(expired) + ", other "
          + std::to_string(other) + ")");
    check(result_not_ok, "timeout: without an error handler, a result that is not ok()");
    check(pool.queue_size() == 0, "timeout: the queue is empty");
    check(pool.outstanding() == 0, "timeout: nothing outstanding");
}

void control()
{
    EventLoop loop;
    PgPool pool{loop, kNowhere, 1, 1};
    pool.start();
    int failed = 0;
    for (int i = 0; i < 100; ++i)
        pool.execute("SELECT 1", {}, [&](std::string_view) { ++failed; });
    loop.run_for(2500ms);
    check(failed == 0 && pool.queue_size() == 100, "control: no limit, no timeout — the queries wait");
}

// ── A server that answers the handshake late ────────────────────────────────

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

std::string message(char type, const std::string& body)
{
    std::string m(1, type);
    const uint32_t len = htonl(static_cast<uint32_t>(body.size() + 4));
    m.append(reinterpret_cast<const char*>(&len), 4);
    return m + body;
}

// The child: one connection; the handshake answered after @p delay; every
// simple query written to @p report (one per line) and answered as done.
[[noreturn]] void serve(int listen_fd, int report, std::chrono::milliseconds delay)
{
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0)
        _exit(2);
    uint32_t len = 0;
    if (!read_n(fd, &len, 4))
        _exit(3);
    std::string startup(ntohl(len) - 4, '\0');
    if (!read_n(fd, startup.data(), startup.size()))
        _exit(3);

    std::this_thread::sleep_for(delay);
    write_all(fd, message('R', std::string("\0\0\0\0", 4)) + message('Z', "I"));

    for (;;) {
        char type = 0;
        if (!read_n(fd, &type, 1) || !read_n(fd, &len, 4))
            _exit(0);
        std::string body(ntohl(len) - 4, '\0');
        if (!read_n(fd, body.data(), body.size()))
            _exit(0);
        if (type == 'X')
            _exit(0);
        if (type == 'Q') {
            const std::string sql = body.substr(0, body.find('\0'));
            write_all(report, sql + "\n");
            write_all(fd, message('C', std::string("SELECT 0\0", 9)) + message('Z', "I"));
        }
    }
}

void nothing_stale_sent()
{
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 4) != 0
        || ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen) != 0) {
        check(false, "stale: a listening socket");
        return;
    }
    int report[2];
    if (::pipe(report) != 0) {
        check(false, "stale: a pipe");
        return;
    }

    // Timeout 2 s; the handshake ends at 2.7 s. "stale a" (t=0) is failed by
    // the watch; "stale b" (t=0.5) is under the limit at the watch's last turn
    // before the handshake and over it when the connection takes it; "fresh"
    // (t=2.6) is sent.
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(report[0]);
        serve(lfd, report[1], 2700ms);
    }
    ::close(lfd);
    ::close(report[1]);

    {
        EventLoop loop;
        const std::string conninfo = "host=127.0.0.1 port=" + std::to_string(ntohs(a.sin_port))
            + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=10";
        PgPool pool{loop, conninfo, 1, 1};
        pool.set_reset({});
        pool.set_queue_timeout(2s);
        pool.start();

        std::string err_a, err_b, err_fresh;
        bool fresh_ok = false;
        pool.execute("SELECT 'stale a'", {}, [&](std::string_view e) { err_a = std::string(e); });
        loop.run_for(500ms);
        pool.execute("SELECT 'stale b'", {}, [&](std::string_view e) { err_b = std::string(e); });
        loop.run_for(2100ms);
        pool.execute("SELECT 'fresh'",
            [&](std::vector<PgResult> r) { fresh_ok = r.size() == 1 && r[0].ok(); },
            [&](std::string_view e) { err_fresh = std::string(e); });
        loop.run_for(1000ms);

        check(err_a.starts_with("not sent: waited longer than 2s"), "stale: the first one failed (" + err_a + ")");
        check(err_b.starts_with("not sent: waited longer than 2s"), "stale: the second one failed (" + err_b + ")");
        check(fresh_ok, "stale: the fresh one answered (" + err_fresh + ")");
    }

    std::string got;
    char buf[512];
    ssize_t r;
    while ((r = ::read(report[0], buf, sizeof buf)) > 0)
        got.append(buf, static_cast<std::size_t>(r));
    ::close(report[0]);
    int status = 0;
    ::waitpid(child, &status, 0);

    check(got.find("stale") == std::string::npos, "stale: the server received none of the stale ones");
    check(got.find("SELECT 'fresh'") != std::string::npos, "stale: the server received the fresh one");
}

} // namespace

int main()
{
    limit();
    timeout();
    nothing_stale_sent();
    control();

    if (g_failed) {
        std::printf("pg_queue_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("pg_queue_test: all passed\n");
    return 0;
}
