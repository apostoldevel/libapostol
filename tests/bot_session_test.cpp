// bot_session_test — BotSession's sign-in outcome, and the pool's handlers of
// BotSession and refresh_service_token() once their owner is gone.
//
//   1. control: the sign-in batch answers a session — valid(), no error;
//   2. signed in, no session in any scope: a failure — last_error() set, a line
//      in the log, and no new sign-in before the retry pause has passed;
//   2a. api.login answers a NULL session: a failure named after the login, never
//      "signed in";
//   3. a BotSession destroyed with its sign-in in flight: the late answer
//      leaves the memory it occupied untouched;
//   4. a ServiceToken destroyed with its request in flight: the same.
//
// A child process plays a server: the sign-in batch is answered by the case its
// client id names ("case<N>"), each one counted on a pipe; daemon.token answers
// a token; the answer to a client id holding "slow" comes 300 ms late.
// std::chrono::steady_clock::now() is defined in this binary — the monotonic
// clock plus an offset the test moves.
//
// Usage: bot_session_test

#include "apostol/bot_session.hpp"
#include "apostol/db_platform.hpp"
#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/pg.hpp"
#include "apostol/service_token.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
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
#include <new>
#include <string>
#include <vector>

using namespace apostol;
using namespace std::chrono_literals;

namespace
{

std::chrono::nanoseconds g_offset{0};

} // namespace

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
        std::printf("bot_session_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("bot_session_test: ok   %s\n", what.c_str());
    }
}

// ─── the server ─────────────────────────────────────────────────────────────

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

// One text column, then each of @p rows; the CommandComplete, no ReadyForQuery.
std::string result(const std::vector<std::string>& rows)
{
    std::string out = message('T', be16(1) + std::string("v\0", 2) + be32(0) + be16(0) + be32(25)
                                       + be16(0xFFFF) + be32(0xFFFFFFFF) + be16(0));
    for (const auto& r : rows)
        out += message('D', be16(1) + be32(static_cast<uint32_t>(r.size())) + r);
    return out + message('C', "SELECT " + std::to_string(rows.size()) + std::string(1, '\0'));
}

// One text column, one row holding NULL — what a function answering a NULL
// field sends.
std::string null_row()
{
    return message('T', be16(1) + std::string("v\0", 2) + be32(0) + be16(0) + be32(25)
                            + be16(0xFFFF) + be32(0xFFFFFFFF) + be16(0))
        + message('D', be16(1) + be32(0xFFFFFFFF))
        + message('C', std::string("SELECT 1\0", 9));
}

const std::string k_ready = message('Z', "I");

// The sessions api.get_sessions answers, by case.
const std::vector<std::vector<std::string>> k_sessions{
    {R"({"session":"bot-s1"})", R"({"session":"bot-s2"})"},   // case0: control
    {},                                                         // case1: none in any scope
    {R"({"session":"bot-s"})"},                                 // case2: slow
    {},                                                         // case3: login answers NULL
};

constexpr std::size_t k_login_null = 3;

[[noreturn]] void serve(int listen_fd, int count_fd)
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
        write_all(fd, message('R', std::string("\0\0\0\0", 4)) + k_ready);

        for (;;) {
            char type = 0;
            if (!read_n(fd, &type, 1) || !read_n(fd, &len, 4))
                break;
            std::string m(ntohl(len) - 4, '\0');
            if (!read_n(fd, m.data(), m.size()) || type == 'X')
                break;
            if (type != 'Q')
                continue;

            if (m.find("slow") != std::string::npos)
                ::usleep(300 * 1000);

            const auto at = m.find("'case");
            if (m.find("api.login(") != std::string::npos && at != std::string::npos) {
                const std::size_t n = std::stoul(m.substr(at + 5));
                write_all(count_fd, "L");
                write_all(fd, (n == k_login_null ? null_row() : result({R"({"session":"login-s"})"}))
                                  + result(n < k_sessions.size() ? k_sessions[n]
                                                                 : std::vector<std::string>{})
                                  + k_ready);
            } else if (m.find("daemon.token(") != std::string::npos) {
                write_all(fd, result({R"({"access_token":"tok","session":"s","expires_in":3600})"})
                                  + k_ready);
            } else {
                write_all(fd, message('C', std::string("SELECT 0\0", 9)) + k_ready);
            }
        }
        ::close(fd);
    }
}

struct Server
{
    pid_t       child{-1};
    int         count_fd{-1};
    std::string conninfo;

    bool start()
    {
        int p[2];
        if (::pipe(p) != 0)
            return false;
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t alen = sizeof a;
        if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0
            || ::listen(lfd, 8) != 0
            || ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen) != 0)
            return false;

        child = ::fork();
        if (child == 0) {
            ::close(p[0]);
            serve(lfd, p[1]);
        }
        ::close(lfd);
        ::close(p[1]);
        count_fd = p[0];
        ::fcntl(count_fd, F_SETFL, ::fcntl(count_fd, F_GETFL) | O_NONBLOCK);

        conninfo = "host=127.0.0.1 port=" + std::to_string(ntohs(a.sin_port))
            + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=5";
        return true;
    }

    // Sign-in batches the server has answered since the last call.
    int logins()
    {
        int  n = 0;
        char buf[64];
        for (;;) {
            const ssize_t r = ::read(count_fd, buf, sizeof buf);
            if (r <= 0)
                return n;
            n += static_cast<int>(r);
        }
    }

    ~Server()
    {
        if (child > 0) {
            ::kill(child, SIGTERM);
            ::waitpid(child, nullptr, 0);
        }
        if (count_fd >= 0)
            ::close(count_fd);
    }
};

struct Capture : LogTarget
{
    std::string* out;
    explicit Capture(std::string* o) : out(o) {}
    void write(LogLevel, std::string_view m) override { out->append(m).append("\n"); }
};

// One sign-in round: refresh_if_needed(), then the loop until the outcome and
// the sign-out of the login session have come back.
void round(EventLoop& loop, BotSession& bot)
{
    bot.refresh_if_needed();
    for (int i = 0; i < 40 && !bot.valid() && bot.last_error().empty(); ++i)
        loop.run_for(50ms);
    loop.run_for(200ms);
}

// ─── 1, 2. the outcome of a sign-in ─────────────────────────────────────────

void sign_in(Server& srv)
{
    std::string said;
    Logger      diag;
    diag.add_target(std::make_unique<Capture>(&said));
    EventLoop::set_diagnostics(&diag);

    {
        EventLoop loop;
        PgPool    pool{loop, srv.conninfo, 1, 1};
        pool.set_reset({});
        pool.start();

        BotSession bot(pool, "BotSessionTest/1.0", "127.0.0.1");
        bot.set_credentials("case0", "secret");
        round(loop, bot);
        check(bot.valid() && bot.sessions().size() == 2 && bot.session() == "bot-s1",
              "control: two sessions, valid");
        check(bot.last_error().empty(), "control: no error");
        check(srv.logins() == 1, "control: one sign-in batch");
    }

    {
        EventLoop loop;
        PgPool    pool{loop, srv.conninfo, 1, 1};
        pool.set_reset({});
        pool.start();

        said.clear();
        BotSession bot(pool, "BotSessionTest/1.0", "127.0.0.1");
        bot.set_credentials("case1", "secret");
        round(loop, bot);
        check(!bot.valid(), "no session in any scope: not valid");
        check(bot.last_error().find("found no session") != std::string::npos,
              "no session in any scope: last_error() says so (" + bot.last_error() + ")");
        check(said.find("BotSession [BotSessionTest/1.0]: no session") != std::string::npos,
              "no session in any scope: written to the log");
        check(srv.logins() == 1, "no session in any scope: one sign-in batch");

        // The heartbeat goes on: within the pause, nothing is sent.
        for (int i = 0; i < 5; ++i)
            round(loop, bot);
        check(srv.logins() == 0, "no session in any scope: no sign-in again within the pause");

        g_offset += 11s;   // past the 10 s pause
        round(loop, bot);
        check(srv.logins() == 1, "no session in any scope: signed in again after the pause");
    }

    {
        EventLoop loop;
        PgPool    pool{loop, srv.conninfo, 1, 1};
        pool.set_reset({});
        pool.start();

        said.clear();
        BotSession bot(pool, "BotSessionTest/1.0", "127.0.0.1");
        bot.set_credentials("case3", "secret");
        round(loop, bot);
        check(!bot.valid(), "login answered no session: not valid");
        check(bot.last_error() == "api.login returned no session",
              "login answered no session: last_error() names the login (" + bot.last_error() + ")");
        check(said.find("signed in") == std::string::npos,
              "login answered no session: the log does not say signed in");
        check(srv.logins() == 1, "login answered no session: one sign-in batch");
    }

    EventLoop::unset_diagnostics(&diag);
}

// ─── 3, 4. the owner gone with a request in flight ──────────────────────────
//
// Run in a child: the object is built in a buffer, destroyed, and the buffer
// filled with a pattern; the late answer must leave the pattern as it was. A
// handler that writes there changes it, or crashes on what it reads — either
// way the child does not exit 0.

constexpr unsigned char k_pattern = 0xA5;

template <typename Build, typename Destroy>
int gone_in_flight(Server& srv, std::size_t size, Build build, Destroy destroy)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        EventLoop loop;
        PgPool    pool{loop, srv.conninfo, 1, 1};
        pool.set_reset({});
        pool.start();

        alignas(std::max_align_t) static unsigned char buf[4096];
        if (size > sizeof buf)
            _exit(3);
        void* obj = build(buf, pool);
        destroy(obj);
        std::memset(buf, k_pattern, size);

        loop.run_for(1000ms);   // the answer comes after 300 ms

        for (std::size_t i = 0; i < size; ++i)
            if (buf[i] != k_pattern)
                _exit(1);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

std::string describe(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? "untouched"
             : WEXITSTATUS(status) == 1 ? "written after destruction"
                                        : "exit " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return std::string("killed by signal ") + std::to_string(WTERMSIG(status));
    return "?";
}

void owner_gone(Server& srv)
{
    const int b = gone_in_flight(
        srv, sizeof(BotSession),
        [](void* at, PgPool& pool) {
            auto* bot = new (at) BotSession(pool, "BotSessionTest/1.0", "127.0.0.1");
            bot->set_credentials("case2-slow", "secret");
            bot->refresh_if_needed();
            return static_cast<void*>(bot);
        },
        [](void* p) { static_cast<BotSession*>(p)->~BotSession(); });
    check(b == 0, "BotSession destroyed in flight: its memory " + describe(b));
    srv.logins();

    Logger log;
    const int t = gone_in_flight(
        srv, sizeof(ServiceToken),
        [&log](void* at, PgPool& pool) {
            auto* token = new (at) ServiceToken();
            db_platform::refresh_service_token(pool, *token, log, "test", "slow", "secret", "",
                                               "agent", "host");
            return static_cast<void*>(token);
        },
        [](void* p) { static_cast<ServiceToken*>(p)->~ServiceToken(); });
    check(t == 0, "ServiceToken destroyed in flight: its memory " + describe(t));
}

} // namespace

int main()
{
    Server srv;
    if (!srv.start()) {
        std::printf("bot_session_test: FAIL a listening socket\n");
        return 1;
    }

    sign_in(srv);
    owner_gone(srv);

    if (g_failed) {
        std::printf("bot_session_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("bot_session_test: all passed\n");
    return 0;
}
