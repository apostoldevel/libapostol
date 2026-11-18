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
//   3. leader lock: connecting on a slow name does not hold the loop;
//   4. resolver: of two names, one that fails keeps its last addresses;
//   5. resolver: after stale() the next string waits for a fresh answer;
//   6. resolver: a lookup that failed is not repeated by every call before
//      the retry pause;
//   7. pool: a connection lost after its name moved reconnects to the new
//      address, never resets to the old one;
//   8. leader lock: a lookup that ends on its own does not connect between
//      two ticks.
//
// Usage: pg_resolve_test

#include "apostol/event_loop.hpp"
#include "apostol/pg.hpp"
#include "apostol/pg_leader.hpp"

#include "../src/pg/pg_resolver.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
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
std::atomic<int>  g_b_mode{0};     // b.pg.test: 0 — 127.0.0.2, 1 — fails, 2 — 127.0.0.3
std::atomic<int>  g_flips{0};      // lookups of flip.pg.test: the first 127.0.0.1, then 127.0.0.2
std::chrono::nanoseconds g_offset{0};
}

// steady_clock::now() is defined in this binary: the monotonic clock plus an
// offset a test moves.
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
    if (n == "a.pg.test")
        return real("127.0.0.1", service, hints, res);
    if (n == "b.pg.test") {
        const int m = g_b_mode;
        if (m == 1)
            return EAI_AGAIN;
        return real(m == 2 ? "127.0.0.3" : "127.0.0.2", service, hints, res);
    }
    if (n == "flip.pg.test")
        return real(g_flips++ == 0 ? "127.0.0.1" : "127.0.0.2", service, hints, res);
    if (n.starts_with("quick.")) {
        std::this_thread::sleep_for(300ms);
        return real("127.0.0.1", service, hints, res);
    }
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

// ── 4–6. the resolver itself ────────────────────────────────────────────────

struct Lookups
{
    int                  done = 0;
    PgResolver::Outcome  last;
};

// Runs the loop until one more lookup has ended, or 3 s.
void await(EventLoop& loop, Lookups& seen)
{
    const int before = seen.done;
    for (int i = 0; i < 300 && seen.done == before; ++i)
        loop.run_for(10ms);
}

void resolver_partial_failure()
{
    g_b_mode = 0;
    EventLoop loop;
    Lookups seen;
    PgResolver r{loop, "host=a.pg.test,b.pg.test port=1 user=t",
                 [&](const PgResolver::Outcome& o) { ++seen.done; seen.last = o; }};
    (void)r.conninfo();
    await(loop, seen);
    const auto first = r.conninfo().value_or("");
    check(first.find("127.0.0.2") != std::string::npos, "partial: both names found (" + first + ")");

    g_b_mode = 1;
    r.stale();
    (void)r.conninfo();
    await(loop, seen);
    const auto after = r.conninfo().value_or("");
    check(after.find("127.0.0.1") != std::string::npos && after.find("127.0.0.2") != std::string::npos,
          "partial: the name that failed keeps its last address (" + after + ")");
    g_b_mode = 0;
}

void resolver_waits_after_stale()
{
    g_b_mode = 0;
    EventLoop loop;
    Lookups seen;
    PgResolver r{loop, "host=b.pg.test port=1 user=t",
                 [&](const PgResolver::Outcome& o) { ++seen.done; seen.last = o; }};
    (void)r.conninfo();
    await(loop, seen);

    g_b_mode = 2;   // the server moved
    r.stale();
    const auto now = r.conninfo();
    check(!now, "stale: the next string waits (" + now.value_or("nullopt") + ")");
    await(loop, seen);
    const auto fresh = r.conninfo().value_or("");
    check(fresh.find("127.0.0.3") != std::string::npos && fresh.find("127.0.0.2") == std::string::npos,
          "stale: then the fresh address (" + fresh + ")");
    g_b_mode = 0;
}

void resolver_failure_not_repeated()
{
    g_b_mode = 0;
    EventLoop loop;
    Lookups seen;
    PgResolver r{loop, "host=b.pg.test port=1 user=t",
                 [&](const PgResolver::Outcome& o) { ++seen.done; seen.last = o; }};
    (void)r.conninfo();
    await(loop, seen);

    g_b_mode = 1;
    g_offset += PgResolver::kMaxAge + 1s;   // the answer is old: a refresh is due
    (void)r.conninfo();
    await(loop, seen);
    check(!seen.last.error.empty(), "failed refresh: the lookup says the name failed (" + seen.last.error + ")");

    g_lookups = 0;
    for (int i = 0; i < 5; ++i) {
        const auto s = r.conninfo();
        check(s && s->find("127.0.0.2") != std::string::npos, "failed refresh: the old address is handed out");
        loop.run_for(50ms);
    }
    check(g_lookups == 0, "failed refresh: not looked up again before the pause ("
                              + std::to_string(g_lookups.load()) + " lookups)");

    g_offset += 6s;   // past the 5 s pause
    (void)r.conninfo();
    await(loop, seen);
    check(g_lookups == 1, "failed refresh: looked up again after the pause");
    g_b_mode = 0;
}

// ── 7. a lost connection after the name moved ───────────────────────────────
//
// One port on 127.0.0.1 and 127.0.0.2. On .1 the first connection is served
// for 2 s and every other one is closed at once — so the pool's second
// connection fails, looks the name up again and lands on .2; then the first
// one is lost. Every connection to .1 after that is reported.

long long ms_now()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int listen_on(const char* ip, int port)
{
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 8) != 0) {
        ::close(lfd);
        return -1;
    }
    return lfd;
}

int free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen    = sizeof a;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &alen);
    ::close(fd);
    return ntohs(a.sin_port);
}

std::string drain(int fd)
{
    std::string got;
    char buf[512];
    ssize_t r;
    while ((r = ::read(fd, buf, sizeof buf)) > 0)
        got.append(buf, static_cast<std::size_t>(r));
    return got;
}

void pool_lost_after_move()
{
    g_flips = 0;
    const int port = free_port();
    const int l1 = listen_on("127.0.0.1", port);
    const int l2 = listen_on("127.0.0.2", port);
    int p[2];
    if (l1 < 0 || l2 < 0 || ::pipe(p) != 0) {
        check(false, "moved: two listeners");
        return;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        long long first = 0;
        pollfd fds[2] = {{l1, POLLIN, 0}, {l2, POLLIN, 0}};
        for (;;) {
            if (::poll(fds, 2, -1) <= 0)
                continue;
            for (int i = 0; i < 2; ++i) {
                if (!(fds[i].revents & POLLIN))
                    continue;
                const int fd = ::accept(fds[i].fd, nullptr, nullptr);
                if (fd < 0)
                    continue;
                if (i == 1 || first == 0) {
                    if (i == 0)
                        first = ms_now();
                    if (::fork() == 0) {
                        if (i == 0)
                            ::alarm(2);   // the first connection to .1 is lost after 2 s
                        session(fd, p[1]);
                    }
                } else if (ms_now() - first > 1900) {
                    write_all(p[1], "late\n");
                }
                ::close(fd);
            }
        }
    }
    ::close(l1);
    ::close(l2);
    ::close(p[1]);
    ::fcntl(p[0], F_SETFL, ::fcntl(p[0], F_GETFL) | O_NONBLOCK);

    {
        EventLoop loop;
        PgPool pool{loop, conninfo("flip.pg.test", port), 2, 2};
        pool.set_reset({});
        pool.start();
        loop.run_for(4500ms);
        bool answered = false;
        pool.execute("SELECT 'after'", [&](std::vector<PgResult> r) { answered = r.size() == 1 && r[0].ok(); },
                     [](std::string_view) {});
        loop.run_for(1000ms);
        check(answered, "moved: the pool still answers");
    }
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    const auto got = drain(p[0]);
    ::close(p[0]);
    const auto late = static_cast<int>(std::count(got.begin(), got.end(), 'l'));
    check(late == 0, "moved: nothing reconnects to the old address once the connection is lost ("
                         + std::to_string(late) + " times)");
}

// ── 8. the leader's attempts follow its ticks ───────────────────────────────

void leader_connects_on_tick()
{
    const int port = free_port();
    const int lfd  = listen_on("127.0.0.1", port);
    int p[2];
    if (lfd < 0 || ::pipe(p) != 0) {
        check(false, "leader ticks: a listener");
        return;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        for (;;) {
            const int fd = ::accept(lfd, nullptr, nullptr);
            if (fd < 0)
                _exit(2);
            write_all(p[1], std::to_string(ms_now()) + "\n");
            ::close(fd);   // every attempt fails
        }
    }
    ::close(lfd);
    ::close(p[1]);
    ::fcntl(p[0], F_SETFL, ::fcntl(p[0], F_GETFL) | O_NONBLOCK);
    {
        EventLoop loop;
        PgLeaderLock lock{loop, conninfo("quick.pg.test", port), "test/leader", "test", 1s, nullptr};
        lock.start([] {}, [](std::string_view) {});
        loop.run_for(5000ms);
    }
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    const auto got = drain(p[0]);
    ::close(p[0]);

    std::vector<long long> at;
    std::size_t pos = 0;
    while (pos < got.size()) {
        const auto nl = got.find('\n', pos);
        if (nl == std::string::npos)
            break;
        at.push_back(std::stoll(got.substr(pos, nl - pos)));
        pos = nl + 1;
    }
    long long closest = 1000000;
    for (std::size_t i = 1; i < at.size(); ++i)
        closest = std::min(closest, at[i] - at[i - 1]);
    check(at.size() >= 2, "leader ticks: it keeps trying (" + std::to_string(at.size()) + " attempts)");
    check(closest >= 700, "leader ticks: no attempt between two ticks (closest "
                              + std::to_string(closest) + " ms apart)");
}

} // namespace

int main()
{
    g_loop_thread = std::this_thread::get_id();
    pool_on_slow_name();
    pool_on_gone_name();
    leader_on_slow_name();
    resolver_partial_failure();
    resolver_waits_after_stale();
    resolver_failure_not_repeated();
    pool_lost_after_move();
    leader_connects_on_tick();

    if (g_failed) {
        std::printf("pg_resolve_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("pg_resolve_test: all passed\n");
    return 0;
}
