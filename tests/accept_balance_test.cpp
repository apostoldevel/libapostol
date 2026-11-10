// accept_balance_test — the slot the master sets aside for a worker before it
// forks it.
//
// First the table itself, this process standing for the master and forked
// children for the workers (no sockets):
//   1. a slot set aside is counted, as holding nothing, by a worker that runs
//      while its owner has not joined yet;
//   2. it is counted for kStartGraceMs, not longer;
//   3. its worker takes it when it joins: what it publishes is what the
//      others see, and no second slot is counted for it;
//   4. it is free again when its worker is reaped before joining, when the
//      fork fails, and when the next reserve() comes before it was resolved.
//
// Then an application, in an edge-triggered build: a master with three
// workers and a custom process "probe", started before the workers. The
// first worker to run starts at once, the others kSlowStartMs later — the
// window in which connections queued on the socket used to be taken whole by
// the first. The probe opens kClients connections at its start and asks each
// which process holds it (GET /pid):
//   5. they are spread over the workers — no worker holds more than
//      ceil(kClients / kWorkers) + 1;
//   6. once every worker runs, the one holding most is killed (SIGKILL);
//      once the master has forked its successor the probe opens as many
//      again — the successor, starting late as well, takes its share, the
//      others do not take them.
// The probe writes its verdict into <dir>/result. A level-triggered build has
// no spreading: it runs steps 1–4 only.
//
// Usage: accept_balance_test -p <dir>/ -c <dir>/accept_balance_test.json

#include "apostol/accept_balance.hpp"
#include "apostol/application.hpp"
#include "apostol/module.hpp"
#include "apostol/process_module.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace apostol;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("accept_balance_test: FAIL %s\n", what.c_str());
    }
}

// ─── 1–4: the table ──────────────────────────────────────────────────────────

/// A worker that never joins (it has not started): it waits to be killed.
pid_t fork_idle(AcceptBalance& b)
{
    b.reserve();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::pause();
        ::_exit(0);
    }
    b.reserved_for(pid);
    return pid;
}

/// A worker that joins at once, holds @p load and runs @p body; its exit code
/// is the verdict (0 — as expected).
template <typename Body>
pid_t fork_worker(AcceptBalance& b, int load, Body body)
{
    b.reserve();
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (!b.join(::getpid()))
            ::_exit(3);
        b.publish(load);
        ::_exit(body(b) ? 0 : 1);
    }
    b.reserved_for(pid);
    return pid;
}

int exit_code(pid_t pid)
{
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

void stop(pid_t pid)
{
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

void table_steps()
{
    std::fflush(stdout);
    // 1 and 2: a slot set aside for a worker that has not joined.
    {
        AcceptBalance b;
        check(b.create(), "mmap");
        const pid_t idle = fork_idle(b);
        const pid_t w = fork_worker(b, 1, [](AcceptBalance& self) {
            if (self.may_accept(1))
                return false;                       // 1: the idle one is not counted
            std::this_thread::sleep_for(AcceptBalance::kStartGraceMs + 200ms);
            return self.may_accept(1);              // 2: counted past the grace
        });
        const int rc = exit_code(w);
        check(rc == 0, fmt::format("1–2: a slot set aside, its worker not joined — exit {} "
                                   "(1: not counted at once, or 2: still counted past the grace)", rc));
        stop(idle);
    }

    // 3: the worker joins the slot set aside for it.
    {
        AcceptBalance b;
        b.create();
        int ready[2];
        if (::pipe(ready) != 0) {
            check(false, "pipe");
            return;
        }
        const pid_t busy = fork_worker(b, 5, [&](AcceptBalance&) {
            const char c = 1;
            (void)!::write(ready[1], &c, 1);
            ::pause();
            return true;
        });
        const pid_t w = fork_worker(b, 1, [&](AcceptBalance& self) {
            // Bounded: a busy that failed to join writes nothing, and the
            // write end stays open in this process.
            pollfd p{ready[0], POLLIN, 0};
            char c = 0;
            if (::poll(&p, 1, 5000) != 1 || ::read(ready[0], &c, 1) != 1)
                return false;
            // The other holds 5: nobody holds fewer than 1. A second slot of its
            // own, set aside and never joined, would read 0.
            return self.may_accept(1);
        });
        const int rc = exit_code(w);
        check(rc == 0, fmt::format("3: a worker publishing 5, seen in the slot set aside for it — exit {}", rc));
        stop(busy);
        ::close(ready[0]);
        ::close(ready[1]);
    }

    // 4: reaped before joining; the fork failed.
    {
        AcceptBalance b;
        b.create();
        const pid_t idle = fork_idle(b);
        stop(idle);
        b.release(idle);
        b.reserve();
        b.reserved_for(-1);                         // as fork_child() on a failed fork
        b.reserve();                                // never handed to a pid —
        b.reserve();                                // freed by the next reserve()
        b.reserved_for(-1);
        const pid_t w = fork_worker(b, 1, [](AcceptBalance& self) { return self.may_accept(1); });
        const int rc = exit_code(w);
        check(rc == 0, fmt::format("4: the slots of a reaped worker, of a failed fork and of a reserve() "
                                   "never resolved are free — exit {}", rc));
    }
}

// ─── 5–6: the application ────────────────────────────────────────────────────
#ifdef APOSTOL_EPOLL_ET   // a level-triggered build spreads nothing


constexpr int  kWorkers     = 3;
constexpr int  kClients     = 9;
constexpr auto kSlowStartMs = 60ms;                 // under AcceptBalance::kDeferMs

fs::path          g_dir;
std::atomic<int>* g_started   = nullptr;            // shared: workers started so far
std::atomic<int>* g_listening = nullptr;            // shared: of them, listening

fs::path result_file() { return g_dir / "result"; }

std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, std::string_view s)
{
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << s;
    }
    fs::rename(tmp, p);
}

/// GET /pid — the pid of the process that holds the connection.
class Pid final : public Module
{
public:
    std::string_view name() const override { return "Pid"; }
    bool enabled() const override { return true; }
    bool execute(const HttpRequest&, HttpResponse& resp) override
    {
        resp.set_status(HttpStatus::ok).set_body(std::to_string(::getpid()));
        return true;
    }
};

std::set<pid_t> children_of(pid_t parent)
{
    std::set<pid_t> out;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream f(it->path() / "stat");
        std::string stat;
        std::getline(f, stat);
        const auto close = stat.rfind(')');
        if (close == std::string::npos || close + 2 >= stat.size())
            continue;
        std::istringstream rest(stat.substr(close + 2));
        char  state = 0;
        pid_t ppid  = 0;
        rest >> state >> ppid;
        if (ppid == parent && state != 'Z')
            out.insert(std::stoi(name));
    }
    return out;
}

class Probe final : public ProcessModule
{
public:
    std::string_view name() const override { return "probe"; }

    void on_start(EventLoop&, Application& app) override
    {
        // The whole scenario here, blocking: the probe's loop has nothing else
        // to do, and the workers are being forked meanwhile.
        app_    = &app;
        master_ = ::getppid();
        port_   = app.settings().server_port;
        const std::string why = run();
        if (why.empty()) {
            app_->logger().notice("probe: PASS {}", log_);
            write_file(result_file(), "PASS " + log_ + "\n");
        } else {
            app_->logger().error("probe: FAIL {} ({})", why, log_);
            write_file(result_file(), "FAIL " + why + " (" + log_ + ")\n");
        }
        ::kill(master_, SIGTERM);
    }

    void on_stop() override
    {
        for (const auto& c : conns_)
            ::close(c.fd);
        conns_.clear();
    }

private:
    struct Conn
    {
        int   fd{-1};
        pid_t holder{0};
    };

    Application*      app_{nullptr};
    pid_t             master_{0};
    int               port_{0};
    std::vector<Conn> conns_;
    std::string       log_;

    std::string open(int n)
    {
        std::vector<Conn> fresh;
        const auto give_up = [&fresh](std::string why) {
            for (const auto& c : fresh)
                ::close(c.fd);
            return why;
        };
        for (int i = 0; i < n; ++i) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                return give_up(fmt::format("socket: {}", std::strerror(errno)));
            fresh.push_back({fd, 0});
            sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_port        = htons(static_cast<uint16_t>(port_));
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
                return give_up(fmt::format("connect: {}", std::strerror(errno)));
        }
        static constexpr std::string_view kGet = "GET /pid HTTP/1.1\r\nHost: probe\r\n\r\n";
        for (auto& c : fresh)
            if (::send(c.fd, kGet.data(), kGet.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(kGet.size()))
                return give_up("send");
        // The body of each answer is the pid of its holder.
        const auto deadline = Clock::now() + 5s;
        for (auto& c : fresh) {
            std::string buf;
            for (;;) {
                const auto head = buf.find("\r\n\r\n");
                if (head != std::string::npos) {
                    const auto cl = buf.find("Content-Length: ");
                    if (cl != std::string::npos && cl < head) {
                        const auto len = std::stoul(buf.substr(cl + 16));
                        if (buf.size() >= head + 4 + len) {
                            c.holder = std::stoi(buf.substr(head + 4, len));
                            break;
                        }
                    }
                }
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                pollfd p{c.fd, POLLIN, 0};
                if (left.count() <= 0 || ::poll(&p, 1, static_cast<int>(left.count())) <= 0)
                    return give_up("no answer in 5 s");
                char tmp[512];
                const ssize_t r = ::recv(c.fd, tmp, sizeof(tmp), 0);
                if (r <= 0)
                    return give_up("connection closed before the answer");
                buf.append(tmp, static_cast<std::size_t>(r));
            }
        }
        conns_.insert(conns_.end(), fresh.begin(), fresh.end());
        return {};
    }

    std::map<pid_t, int> spread() const
    {
        std::map<pid_t, int> by;
        for (const auto& c : conns_)
            ++by[c.holder];
        return by;
    }

    static std::string show(const std::map<pid_t, int>& by)
    {
        std::string s;
        for (const auto& [pid, n] : by)
            s += fmt::format("{}{}", s.empty() ? "" : "/", n);
        return s;
    }

    std::string run()
    {
        constexpr int kBound = (kClients + kWorkers - 1) / kWorkers + 1;

        // 5. Queued while the workers start.
        if (auto e = open(kClients); !e.empty())
            return "5: " + e;
        auto by = spread();
        log_ = "start " + show(by);
        int   top     = 0;
        pid_t busiest = 0;
        for (const auto& [pid, n] : by)
            if (n > top) {
                top     = n;
                busiest = pid;
            }
        if (top > kBound)
            return fmt::format("5: a worker took {} of {} queued while the workers started (bound {})",
                               top, kClients, kBound);

        // 6. Once every worker runs (its first publish lands 1 ms after its
        // loop starts — 50 ms is ample), the busiest killed; its successor
        // starts late, as the others did.
        for (const auto deadline = Clock::now() + 10s;
             g_listening->load() < kWorkers && Clock::now() < deadline;)
            std::this_thread::sleep_for(1ms);
        if (g_listening->load() < kWorkers)
            return "6: the workers did not all start in 10 s";
        std::this_thread::sleep_for(50ms);
        const auto before = children_of(master_);
        ::kill(busiest, SIGKILL);
        std::erase_if(conns_, [busiest](const Conn& c) {
            if (c.holder != busiest)
                return false;
            ::close(c.fd);
            return true;
        });
        pid_t successor = 0;
        for (const auto deadline = Clock::now() + 10s; !successor && Clock::now() < deadline;) {
            for (pid_t p : children_of(master_))
                if (!before.count(p))
                    successor = p;
            if (!successor)
                std::this_thread::sleep_for(1ms);
        }
        if (!successor)
            return "6: no worker forked after the kill in 10 s";
        if (auto e = open(top); !e.empty())
            return "6: " + e;
        by = spread();
        log_ += ", after kill -9 " + show(by);
        for (const auto& [pid, n] : by)
            if (n > kBound)
                return fmt::format("6: a worker holds {} after {} reconnected while its successor "
                                   "started (bound {})", n, top, kBound);
        if (!by.count(successor))
            return "6: the successor took none";
        return {};
    }
};

class TestApp final : public Application
{
public:
    TestApp() : Application("accept_balance_test") {}

protected:
    void on_worker_start(EventLoop& loop) override
    {
        // The first worker starts at once, every later one late.
        if (g_started->fetch_add(1) > 0)
            std::this_thread::sleep_for(kSlowStartMs);
        module_manager().add_module(std::make_unique<Pid>());
        start_http_server(loop, settings().server_port);
        g_listening->fetch_add(1);
    }

    void create_custom_processes() override
    {
#ifdef WITH_POSTGRESQL
        add_custom_process(std::unique_ptr<ProcessModule>(std::make_unique<Probe>()));
#else
        add_custom_process("probe", [this](EventLoop& loop) {
            static Probe probe;
            probe.on_start(loop, *this);
        });
#endif
    }
};

#endif // APOSTOL_EPOLL_ET

} // namespace

int main(int argc, char* argv[])
{
    table_steps();
    if (g_failed) {
        std::printf("accept_balance_test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("accept_balance_test: table — PASS\n");
    std::fflush(stdout);   // before the forks: a child would print it again

#ifndef APOSTOL_EPOLL_ET
    (void)argc;
    (void)argv;
    std::printf("accept_balance_test: a level-triggered build spreads nothing — steps 5 and 6 not run\n");
    return 0;
#else
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "-p")
            g_dir = argv[i + 1];
    if (g_dir.empty()) {
        std::fprintf(stderr, "usage: accept_balance_test -p <dir>/ -c <dir>/accept_balance_test.json\n");
        return 2;
    }

    void* shared = ::mmap(nullptr, 2 * sizeof(std::atomic<int>), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        std::fprintf(stderr, "accept_balance_test: mmap\n");
        return 2;
    }
    g_started   = new (shared) std::atomic<int>(0);
    g_listening = new (static_cast<std::atomic<int>*>(shared) + 1) std::atomic<int>(0);

    int port = 0;
    if (int fd = ::socket(AF_INET, SOCK_STREAM, 0); fd >= 0) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len     = sizeof(a);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0
            && ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
            port = ntohs(a.sin_port);
        ::close(fd);
    }
    if (port == 0) {
        std::fprintf(stderr, "accept_balance_test: no free port\n");
        return 2;
    }

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "logs");
    write_file(g_dir / "accept_balance_test.json", fmt::format(R"({{
  "workers": {},
  "process": {{ "master": true, "helper": false, "stop_timeout": 5 }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice" }}
}}
)", kWorkers, port));

    int rc = 0;
    {
        TestApp app;
        rc = app.run(argc, argv);
    }

    const auto verdict = read_file(result_file());
    std::printf("accept_balance_test: %s", verdict.empty() ? "no verdict — the probe did not finish\n"
                                                           : verdict.c_str());
    std::printf("accept_balance_test: master exit %d; log %s\n", rc, (g_dir / "logs/error.log").c_str());
    return rc == 0 && verdict.rfind("PASS", 0) == 0 ? 0 : 1;
#endif
}
