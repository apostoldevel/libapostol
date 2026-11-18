// pg_server_test — PgPool against a scripted server (a child process speaking
// just enough of the protocol; every connection in its own child):
//
//   1. cancel() of a finished query finds nothing and returns false, every
//      time; cancel() of a queued one returns true, and the query never
//      reaches the server nor its handler;
//   2. a notification handler that throws: the other handler of the channel
//      gets every notification of the batch, the thrower is called for each,
//      and the listener is the same — one LISTEN on the server;
//   3. an on_ready handler that throws: the listener is not replaced — one
//      LISTEN on the server;
//   4. a listener statement larger than the socket takes at once: the server
//      reads it late, gets all of it, and the answer reaches the caller.
//
// Usage: pg_server_test

#include "apostol/event_loop.hpp"
#include "apostol/pg.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
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
        std::printf("pg_server_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("pg_server_test: ok   %s\n", what.c_str());
    }
}

// ── The server ──────────────────────────────────────────────────────────────

constexpr std::size_t kBig = 32u << 20;   // a statement no socket takes at once
constexpr int kNotifies = 5;              // sent behind the answer to LISTEN "ch"

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

std::string done(const char* tag)
{
    return message('C', std::string(tag) + '\0') + message('Z', "I");
}

// One connection: every simple query reported (one line each, a long one by
// its length) and answered as done. A LISTEN on "ch" is followed, in the same
// write, by kNotifies notifications p0…p4. A statement past 1 MiB is read
// only after 500 ms: by then the client's socket is full.
[[noreturn]] void session(int fd, int report)
{
    uint32_t len = 0;
    if (!read_n(fd, &len, 4))
        _exit(3);
    std::string startup(ntohl(len) - 4, '\0');
    if (!read_n(fd, startup.data(), startup.size()))
        _exit(3);
    write_all(fd, message('R', std::string("\0\0\0\0", 4)) + message('Z', "I"));
    for (;;) {
        char type = 0;
        if (!read_n(fd, &type, 1) || !read_n(fd, &len, 4))
            _exit(0);
        const std::size_t n = ntohl(len) - 4;
        if (n > (1u << 20))
            std::this_thread::sleep_for(500ms);
        std::string body(n, '\0');
        if (!read_n(fd, body.data(), body.size()) || type == 'X')
            _exit(0);
        if (type != 'Q')
            continue;
        const std::string sql = body.substr(0, body.find('\0'));
        if (sql.size() > (1u << 20)) {
            write_all(report, "big " + std::to_string(sql.size()) + "\n");
            write_all(fd, done("SELECT 1"));
            continue;
        }
        write_all(report, sql + "\n");
        if (sql.starts_with("LISTEN")) {
            std::string out = done("LISTEN");
            if (sql.find("\"ch\"") != std::string::npos) {
                for (int i = 0; i < kNotifies; ++i) {
                    std::string b("\0\0\x10\x92", 4);   // backend pid
                    b += "ch";
                    b += '\0';
                    b += "p" + std::to_string(i);
                    b += '\0';
                    out += message('A', b);
                }
            }
            write_all(fd, out);
        } else {
            write_all(fd, done("SELECT 0"));
        }
    }
}

struct Server
{
    pid_t pid = -1;
    int   port = 0;
    int   report = -1;

    bool start()
    {
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t alen    = sizeof a;
        int p[2];
        if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 8) != 0
            || ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen) != 0 || ::pipe(p) != 0)
            return false;
        port = ntohs(a.sin_port);
        pid  = ::fork();
        if (pid == 0) {
            ::close(p[0]);
            for (;;) {
                const int fd = ::accept(lfd, nullptr, nullptr);
                if (fd < 0)
                    _exit(2);
                if (::fork() == 0)
                    session(fd, p[1]);
                ::close(fd);
            }
        }
        ::close(lfd);
        ::close(p[1]);
        report = p[0];
        return pid > 0;
    }

    // Called once every pool is gone: the sessions hold the pipe until their
    // connections close.
    std::string stop()
    {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        std::string got;
        char buf[512];
        ssize_t r;
        while ((r = ::read(report, buf, sizeof buf)) > 0)
            got.append(buf, static_cast<std::size_t>(r));
        ::close(report);
        return got;
    }

    std::string conninfo() const
    {
        return "host=127.0.0.1 port=" + std::to_string(port)
             + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=10";
    }
};

int count_lines(const std::string& got, std::string_view prefix)
{
    int n = 0;
    std::size_t pos = 0;
    while (pos < got.size()) {
        const std::size_t end = got.find('\n', pos);
        const std::string_view line(got.data() + pos, (end == std::string::npos ? got.size() : end) - pos);
        if (line.starts_with(prefix))
            ++n;
        pos = end == std::string::npos ? got.size() : end + 1;
    }
    return n;
}

// Run the loop until @p done or @p limit.
template <typename F>
void run_until(EventLoop& loop, F done, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < until)
        loop.run_for(10ms);
}

// ── 1. cancel() ─────────────────────────────────────────────────────────────

void cancel()
{
    Server srv;
    if (!srv.start()) {
        check(false, "cancel: a server");
        return;
    }
    int answered = 0, canceled_called = 0, cancel_true = 0;
    {
        EventLoop loop;
        PgPool pool{loop, srv.conninfo(), 1, 1};
        pool.start();

        // Queued while the connection is still being made.
        pool.execute("SELECT 'first'", [&](std::vector<PgResult>) { ++answered; });
        const auto id = pool.execute("SELECT 'canceled'", [&](std::vector<PgResult>) { ++canceled_called; },
                                     [&](std::string_view) { ++canceled_called; });
        pool.execute("SELECT 'third'", [&](std::vector<PgResult>) { ++answered; });
        check(pool.cancel(id), "cancel: a queued query is found");

        std::vector<PgPool::QueryId> ids;
        for (int i = 0; i < 100; ++i)
            ids.push_back(pool.execute("SELECT " + std::to_string(i), [&](std::vector<PgResult>) { ++answered; }));
        run_until(loop, [&] { return answered == 102; }, 5s);
        check(answered == 102, "cancel: every other query answered (" + std::to_string(answered) + ")");

        for (auto finished : ids)
            cancel_true += pool.cancel(finished) ? 1 : 0;
        cancel_true += pool.cancel(id) ? 1 : 0;   // dropped by now
        loop.run_for(100ms);
    }
    const std::string got = srv.stop();
    check(cancel_true == 0, "cancel: a finished or dropped query is not found (true "
          + std::to_string(cancel_true) + " times of 101)");
    check(canceled_called == 0, "cancel: the canceled query's handlers are not called");
    check(got.find("'canceled'") == std::string::npos, "cancel: the canceled query never reaches the server");
    check(got.find("'third'") != std::string::npos, "cancel: the query behind it does");
}

// ── 2, 3. A handler that throws ─────────────────────────────────────────────

void notify_handler_throws()
{
    Server srv;
    if (!srv.start()) {
        check(false, "notify: a server");
        return;
    }
    int thrower = 0;
    std::vector<std::string> other;
    {
        EventLoop loop;
        PgPool pool{loop, srv.conninfo(), 1, 1};
        pool.listen("ch", [&](std::string_view, std::string_view payload) {
            ++thrower;
            if (payload == "p1")
                throw std::runtime_error("handler failed");
        });
        pool.listen("ch", [&](std::string_view, std::string_view payload) { other.emplace_back(payload); });
        run_until(loop, [&] { return other.size() >= static_cast<std::size_t>(kNotifies); }, 3s);
        loop.run_for(1500ms);   // time for a replacement listener, if there is one
    }
    const std::string got = srv.stop();
    std::string seen;
    for (const auto& p : other)
        seen += p + " ";
    check(other.size() == static_cast<std::size_t>(kNotifies),
          "notify: the other handler got the batch once (" + seen + ")");
    check(thrower == kNotifies, "notify: the thrower was called for each (" + std::to_string(thrower) + ")");
    check(count_lines(got, "LISTEN") == 1,
          "notify: the listener was not replaced (LISTEN " + std::to_string(count_lines(got, "LISTEN")) + " times)");
}

void ready_handler_throws()
{
    Server srv;
    if (!srv.start()) {
        check(false, "ready: a server");
        return;
    }
    int ready = 0;
    {
        EventLoop loop;
        PgPool pool{loop, srv.conninfo(), 1, 1};
        pool.listen("rd", [](std::string_view, std::string_view) {}, [&](std::string_view) {
            ++ready;
            throw std::runtime_error("on_ready failed");
        });
        run_until(loop, [&] { return ready > 0; }, 3s);
        loop.run_for(2500ms);
    }
    const std::string got = srv.stop();
    check(ready == 1, "ready: on_ready called once (" + std::to_string(ready) + ")");
    check(count_lines(got, "LISTEN") == 1,
          "ready: the listener was not replaced (LISTEN " + std::to_string(count_lines(got, "LISTEN")) + " times)");
}

// ── 4. A statement the socket does not take at once ─────────────────────────

void big_listener_statement()
{
    Server srv;
    if (!srv.start()) {
        check(false, "big: a server");
        return;
    }
    bool answered = false;
    std::string error;
    std::size_t sent = 0;
    {
        EventLoop loop;
        PgPool pool{loop, srv.conninfo(), 1, 1};
        std::string sql = "SELECT '" + std::string(kBig, 'x') + "'";
        sent = sql.size();
        pool.listener_execute(std::move(sql),
            [&](std::vector<PgResult> r) { answered = r.size() == 1 && r[0].ok(); },
            [&](std::string_view e) { error = std::string(e); });
        run_until(loop, [&] { return answered || !error.empty(); }, 5s);
    }
    const std::string got = srv.stop();
    check(got.find("big " + std::to_string(sent)) != std::string::npos,
          "big: the server got all " + std::to_string(sent) + " bytes");
    check(answered, "big: the answer reached the caller" + (error.empty() ? std::string() : " (error: " + error + ")"));
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    cancel();
    notify_handler_throws();
    ready_handler_throws();
    big_listener_statement();
    if (g_failed) {
        std::printf("pg_server_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("pg_server_test: all checks passed\n");
    return 0;
}
