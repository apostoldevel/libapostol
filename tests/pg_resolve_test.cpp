// pg_resolve_test — PgPool and PgLeaderLock look host names up off the loop.
//
// getaddrinfo() is replaced in this binary (libpq's calls reach it too): a
// name under ".pg.test" answers after a delay with 127.0.0.1 — "slow" after
// 1.5 s, "gone" never resolves. Every call is counted by thread.
//
//   1. pool: a pool and its listener on a slow name — start(), execute() and
//      listen() return at once, the loop's 10 ms timer is never late by more
//      than 300 ms, the query reaches a server on 127.0.0.1 and is answered,
//      and the loop's thread looks nothing up;
//   2. a name that does not resolve: the queries wait (as for a server that is
//      down), the loop is not held, the lookup is asked again by the backoff;
//   3. leader lock: connecting on a slow name does not hold the loop.
//
// Usage: pg_resolve_test

#include "apostol/event_loop.hpp"
#include "apostol/pg.hpp"
#include "apostol/pg_leader.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
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
std::thread::id   g_loop_thread;
std::atomic<int>  g_on_loop{0};    // lookups of a test name on the loop's thread
std::atomic<int>  g_lookups{0};    // lookups of a test name, any thread
}

extern "C" int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res)
{
    using Real = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
    static Real real = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "getaddrinfo"));

    const std::string_view n = node ? node : "";
    if (!n.ends_with(".pg.test"))
        return real(node, service, hints, res);

    ++g_lookups;
    if (std::this_thread::get_id() == g_loop_thread)
        ++g_on_loop;
    if (n.starts_with("gone."))
        return EAI_NONAME;
    std::this_thread::sleep_for(1500ms);
    return real("127.0.0.1", service, hints, res);
}

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("pg_resolve_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("pg_resolve_test: ok   %s\n", what.c_str());
    }
}

// The loop's 10 ms timer: the longest gap between two of its turns.
struct Gap
{
    EventLoop&                            loop;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::chrono::milliseconds             worst{0};
    EventLoop::TimerId                    id;

    explicit Gap(EventLoop& l) : loop(l)
    {
        id = loop.add_timer(10ms, [this] {
            const auto now = std::chrono::steady_clock::now();
            worst = std::max(worst, std::chrono::duration_cast<std::chrono::milliseconds>(now - last));
            last  = now;
        });
    }
    ~Gap() { loop.cancel_timer(id); }
};

// ── A server: every connection in its own child; queries answered as done ──

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
        std::string body(ntohl(len) - 4, '\0');
        if (!read_n(fd, body.data(), body.size()) || type == 'X')
            _exit(0);
        if (type == 'Q') {
            write_all(report, body.substr(0, body.find('\0')) + "\n");
            write_all(fd, message('C', std::string("SELECT 0\0", 9)) + message('Z', "I"));
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

    std::string stop()
    {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        std::string got;
        char buf[512];
        ssize_t r;
        // The sessions hold the pipe until their connections close.
        while ((r = ::read(report, buf, sizeof buf)) > 0)
            got.append(buf, static_cast<std::size_t>(r));
        ::close(report);
        return got;
    }
};

std::string conninfo(std::string_view host, int port)
{
    return "host=" + std::string(host) + " port=" + std::to_string(port)
         + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=10";
}

void pool_on_slow_name()
{
    Server srv;
    if (!srv.start()) {
        check(false, "pool: a server");
        return;
    }
    g_on_loop = 0;
    std::string got;
    {
        EventLoop loop;
        Gap gap{loop};
        PgPool pool{loop, conninfo("slow.pg.test", srv.port), 1, 1};
        pool.set_reset({});

        bool answered = false;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        pool.start();
        pool.execute("SELECT 'slow'",
            [&](std::vector<PgResult> r) { answered = r.size() == 1 && r[0].ok(); },
            [&](std::string_view e) { err = std::string(e); });
        pool.listen("ch", [](std::string_view, std::string_view) {});
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        check(took < 100ms, "pool: start(), execute(), listen() return at once (" + std::to_string(took.count()) + " ms)");

        loop.run_for(3000ms);
        check(gap.worst < 300ms, "pool: the loop is never held (worst gap " + std::to_string(gap.worst.count()) + " ms)");
        check(answered, "pool: the query is answered (" + err + ")");
        check(pool.listener_up(), "pool: the listener is up");
    }
    got = srv.stop();
    check(got.find("SELECT 'slow'") != std::string::npos, "pool: the server received the query");
    check(g_on_loop == 0, "pool: nothing looked up on the loop's thread (" + std::to_string(g_on_loop.load()) + ")");
}

void pool_on_gone_name()
{
    g_on_loop  = 0;
    g_lookups  = 0;
    EventLoop loop;
    Gap gap{loop};
    PgPool pool{loop, conninfo("gone.pg.test", 1), 1, 1};
    pool.start();
    int failed = 0;
    for (int i = 0; i < 10; ++i)
        pool.execute("SELECT 1", {}, [&](std::string_view) { ++failed; });
    loop.run_for(3500ms);
    check(failed == 0 && pool.queue_size() == 10, "gone: the queries wait, as for a server that is down");
    check(gap.worst < 300ms, "gone: the loop is never held (worst gap " + std::to_string(gap.worst.count()) + " ms)");
    check(g_lookups >= 2 && g_lookups <= 4,
          "gone: looked up again by the backoff (" + std::to_string(g_lookups.load()) + " times)");
    check(g_on_loop == 0, "gone: nothing looked up on the loop's thread");
}

void leader_on_slow_name()
{
    g_on_loop = 0;
    EventLoop loop;
    Gap gap{loop};
    // Port 1: the connect is refused after the lookup — only the loop matters here.
    PgLeaderLock lock{loop, conninfo("slow.pg.test", 1), "test/leader", "test", 1s, nullptr};
    const auto t0 = std::chrono::steady_clock::now();
    lock.start([] {}, [](std::string_view) {});
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    loop.run_for(2500ms);
    check(took < 100ms, "leader: start() returns at once (" + std::to_string(took.count()) + " ms)");
    check(gap.worst < 300ms, "leader: the loop is never held (worst gap " + std::to_string(gap.worst.count()) + " ms)");
    check(g_on_loop == 0, "leader: nothing looked up on the loop's thread");
}

} // namespace

int main()
{
    g_loop_thread = std::this_thread::get_id();
    pool_on_slow_name();
    pool_on_gone_name();
    leader_on_slow_name();

    if (g_failed) {
        std::printf("pg_resolve_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("pg_resolve_test: all passed\n");
    return 0;
}
