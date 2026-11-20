// notify_metrics_test — the rows a NotifySubscriber gives its process's
// metrics, against a scripted server (a child process speaking just enough of
// the protocol; every connection in its own child).
//
// The server answers the subscription with number 7 and a channel policy of
// the mode the subscriber expects; every other statement is done with no
// rows (a pool's RESET before it, with its own completion). Once the subscriber holds its number, its policy matches and the
// server confirmed LISTEN on its personal channel, the process's answer has:
//
//   - notify_subscribed 1, notify_in_progress 0, notify_handled_total 0 for
//     each result, notify_retries_total 0 — by the channel's code;
//   - db_listen 1 for the personal channel, by the channel's code with
//     listen="personal", not by the name that carries the number.
//
// Usage: notify_metrics_test

#include "apostol/application.hpp"
#include "apostol/event_loop.hpp"
#include "apostol/notify_subscriber.hpp"
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
#include <sstream>
#include <string>
#include <vector>

using namespace apostol;
using db_platform::NotifyRow;
using db_platform::NotifySubscriber;
using namespace std::chrono_literals;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("notify_metrics_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

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
            _exit(4);
        off += static_cast<std::size_t>(w);
    }
}

std::string be16(uint16_t v) { v = htons(v); return std::string(reinterpret_cast<const char*>(&v), 2); }
std::string be32(uint32_t v) { v = htonl(v); return std::string(reinterpret_cast<const char*>(&v), 4); }

std::string message(char type, const std::string& body)
{
    return std::string(1, type) + be32(static_cast<uint32_t>(body.size() + 4)) + body;
}

std::string done(const char* tag)
{
    return message('C', std::string(tag) + '\0') + message('Z', "I");
}

// One row of text columns: RowDescription, DataRow, CommandComplete, ReadyForQuery.
std::string row(const std::vector<std::string>& values)
{
    std::string desc = be16(static_cast<uint16_t>(values.size()));
    std::string data = be16(static_cast<uint16_t>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) {
        desc += "c" + std::to_string(i) + '\0' + be32(0) + be16(0) + be32(25) + be16(0xFFFF)
              + be32(0xFFFFFFFF) + be16(0);
        data += be32(static_cast<uint32_t>(values[i].size())) + values[i];
    }
    return message('T', desc) + message('D', data) + done("SELECT 1");
}

[[noreturn]] void session(int fd)
{
    uint32_t len = 0;
    if (!read_n(fd, &len, 4))
        _exit(3);
    std::string startup(ntohl(len) - 4, '\0');
    if (!read_n(fd, startup.data(), startup.size()))
        _exit(3);
    write_all(fd, message('R', be32(0)) + message('Z', "I"));
    for (;;) {
        char type = 0;
        if (!read_n(fd, &type, 1) || !read_n(fd, &len, 4))
            _exit(0);
        std::string body(ntohl(len) - 4, '\0');
        if (!read_n(fd, body.data(), body.size()) || type == 'X')
            _exit(0);
        if (type != 'Q')
            continue;
        const std::string sql = body.substr(0, body.find('\0'));
        // A pool's query comes behind its RESET, in the same message.
        if (sql.starts_with("RESET"))
            write_all(fd, message('C', std::string("RESET") + '\0'));
        if (sql.starts_with("LISTEN"))
            write_all(fd, done("LISTEN"));
        else if (sql.find("notify.subscribe(") != std::string::npos)
            write_all(fd, row({"7", "1700000000"}));
        else if (sql.find("notify.channel_policy(") != std::string::npos)
            write_all(fd, row({"single", "t", "", "30", "60", "60"}));
        else
            write_all(fd, done("SELECT 0"));
    }
}

struct Server
{
    pid_t pid  = -1;
    int   port = 0;

    bool start()
    {
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t alen    = sizeof a;
        if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 8) != 0
            || ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen) != 0)
            return false;
        port = ntohs(a.sin_port);
        pid  = ::fork();
        if (pid == 0) {
            ::signal(SIGCHLD, SIG_IGN);
            for (;;) {
                const int fd = ::accept(lfd, nullptr, nullptr);
                if (fd < 0)
                    _exit(2);
                if (::fork() == 0)
                    session(fd);
                ::close(fd);
            }
        }
        ::close(lfd);
        return pid > 0;
    }

    void stop()
    {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
    }

    std::string conninfo() const
    {
        return "host=127.0.0.1 port=" + std::to_string(port)
             + " user=t dbname=t sslmode=disable gssencmode=disable connect_timeout=10";
    }
};

class TestApp final : public Application
{
public:
    TestApp() : Application("notify_metrics_test") {}

    void setup(EventLoop& loop, std::string conninfo)
    {
        settings_.pg_conninfo_helper = std::move(conninfo);
        settings_.pg_pool_min        = 1;
        settings_.pg_pool_max        = 1;
        set_worker_loop(loop);
        enable_metrics("t");
    }
};

// The sample of `name` whose labels end with `labels`, with its value.
bool has_sample(const std::string& text, const std::string& name, const std::string& labels,
                const std::string& value)
{
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with(name + "{"))
            continue;
        if (line.find(labels + "} " + value) != std::string::npos
            && line.size() == line.find(labels + "} " + value) + labels.size() + 2 + value.size())
            return true;
    }
    return false;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Server srv;
    if (!srv.start()) {
        std::printf("notify_metrics_test: FAIL the scripted server did not start\n");
        return 1;
    }

    std::string text;
    {
        EventLoop loop;
        TestApp   app;
        app.setup(loop, srv.conninfo());
        PgPool& pool = app.db_pool("helper");

        NotifySubscriber::Options opts;
        opts.channel = "jobs";
        opts.node    = "n1";
        NotifySubscriber sub(pool, loop, app.logger(), opts, [](const NotifyRow&) {});
        sub.start();

        const auto until = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < until
               && !(sub.number() == 7 && sub.policy_ok() && pool.listening("jobs:7")))
            loop.run_for(10ms);
        check(sub.number() == 7 && sub.policy_ok() && pool.listening("jobs:7"),
              "subscribed: number 7, the policy matches, LISTEN on the personal channel confirmed");

        text = app.process_metrics();
    }
    srv.stop();

    check(has_sample(text, "t_notify_subscribed", "channel=\"jobs\"", "1"), "notify_subscribed 1 by the channel");
    check(has_sample(text, "t_notify_in_progress", "channel=\"jobs\"", "0"), "notify_in_progress 0");
    for (const char* result : {"success", "error", "failed"})
        check(has_sample(text, "t_notify_handled_total", std::string("channel=\"jobs\",result=\"") + result + "\"", "0"),
              std::string("notify_handled_total ") + result + " 0");
    check(has_sample(text, "t_notify_retries_total", "channel=\"jobs\"", "0"), "notify_retries_total 0");
    check(has_sample(text, "t_db_listen", "pool=\"helper\",channel=\"jobs\",listen=\"personal\"", "1"),
          "db_listen 1 for the personal channel, by its code");
    check(text.find("jobs:7") == std::string::npos, "the name with the number is in no row");

    if (g_failed)
        std::printf("%s", text.c_str());
    std::printf("notify_metrics_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
