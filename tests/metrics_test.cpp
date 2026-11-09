// metrics_test — the metrics of a node, collected over the channels between its
// processes.
//
// First the text itself, in this process: escaping, number formatting, and
// merge_metrics() grouping the samples of one family from several texts under
// one HELP and TYPE.
//
// Then an application: a master with two workers, the helper and a custom
// process "probe" that drives the scenario and writes its verdict into
// <dir>/result. No database, no HTTP.
//
//   --mode on   enable_metrics("t"); each worker adds a row of its own.
//     1. a collection answers for every live process: t_process_up 1 for each,
//        one TYPE per family and its samples together, a worker's own row
//        with its labels, the channels' own counters of every process;
//     2. the helper held busy past the timeout: t_process_up 0 for it and none
//        of its rows; once it is free again, 1 and its rows are back.
//   --mode off  neither enable_ipc() nor enable_metrics(): no channels at all
//     — every process holds what it was started with and the listening socket,
//     nothing more; a send is refused, a gather reaches only the process
//     itself, a collection is an empty text.
//
// Usage: metrics_test --mode on|off -p <dir>/ -c <dir>/metrics_test.json

#include "apostol/application.hpp"
#include "apostol/ipc.hpp"
#include "apostol/metrics.hpp"
#include "apostol/module.hpp"
#include "apostol/process_module.hpp"

#include <fmt/format.h>

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
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

constexpr int kWorkers = 2;
constexpr int kAll     = kWorkers + 2;   // + helper + probe

fs::path g_dir;
bool     g_on           = true;
int      g_base_sockets = 0;

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

int sockets_of(pid_t pid)
{
    std::error_code ec;
    fs::directory_iterator it(fmt::format("/proc/{}/fd", pid), ec), end;
    if (ec)
        return -1;
    int n = 0;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return -1;
        std::error_code e2;
        const auto target = fs::read_symlink(it->path(), e2).string();
        if (!e2 && target.rfind("socket:", 0) == 0)
            ++n;
    }
    return n;
}

// The children of @p parent, from /proc.
std::vector<pid_t> children_of(pid_t parent)
{
    std::vector<pid_t> out;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        const auto stat  = read_file(it->path() / "stat");
        const auto close = stat.rfind(')');
        if (close == std::string::npos)
            continue;
        std::istringstream in(stat.substr(close + 2));
        char state = 0;
        pid_t ppid = 0;
        in >> state >> ppid;
        if (ppid == parent && state != 'Z')
            out.push_back(static_cast<pid_t>(std::stol(name)));
    }
    return out;
}

std::vector<std::string> lines_of(std::string_view text)
{
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}

std::string family_of_line(const std::string& line)
{
    if (line.rfind("# ", 0) == 0) {
        std::istringstream in(line.substr(7));
        std::string name;
        in >> name;
        return name;
    }
    return line.substr(0, line.find_first_of("{ "));
}

// ─── The text, in this process ───────────────────────────────────────────────

std::string check_text()
{
    {
        MetricsText m("x", {{"role", "w"}});
        m.gauge("g", "a \\ b\nc", 1, {{"l", "q\"b\\s\nn"}});
        m.gauge("g", "ignored", 0.5);
        m.gauge("nan", "", std::nan(""));
        m.counter("c_total", "C.", 3);
        const std::string want =
            "# HELP x_g a \\\\ b\\nc\n"
            "# TYPE x_g gauge\n"
            "x_g{role=\"w\",l=\"q\\\"b\\\\s\\nn\"} 1\n"
            "x_g{role=\"w\"} 0.5\n"
            "# TYPE x_nan gauge\n"
            "x_nan{role=\"w\"} NaN\n"
            "# HELP x_c_total C.\n"
            "# TYPE x_c_total counter\n"
            "x_c_total{role=\"w\"} 3\n";
        if (m.str() != want)
            return "MetricsText:\n" + m.str() + "--- want:\n" + want;
    }
    {
        const std::string a = "# HELP a A.\n# TYPE a gauge\na 1\n"
                              "# TYPE h histogram\nh_bucket{le=\"1\"} 1\nh_sum 1\nh_count 1\n"
                              "# a comment\n";
        const std::string b = "# HELP a other\n# TYPE a gauge\na{x=\"2\"} 2\n"
                              "h_bucket{le=\"1\"} 3\nloose 4\n";
        const std::string want = "# HELP a A.\n# TYPE a gauge\na 1\na{x=\"2\"} 2\n"
                                 "# TYPE h histogram\nh_bucket{le=\"1\"} 1\nh_sum 1\nh_count 1\n"
                                 "h_bucket{le=\"1\"} 3\nloose 4\n";
        const auto got = merge_metrics({a, b});
        if (got != want)
            return "merge_metrics:\n" + got + "--- want:\n" + want;
    }
    return {};
}

// ─── The node ────────────────────────────────────────────────────────────────

void install(Application& app)
{
    app.ipc().on("who", [&app](const IpcPeer&, std::string_view, IpcReply r) {
        r.send(app.ipc().self().name);
    });
    // Busy elsewhere: the loop is held, nothing is read meanwhile.
    app.ipc().on("stall", [](const IpcPeer&, std::string_view b, IpcReply) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(std::string(b))));
    });
}

class Probe final : public ProcessModule
{
public:
    std::string_view name() const override { return "probe"; }

    void on_start(EventLoop& loop, Application& app) override
    {
        app_    = &app;
        loop_   = &loop;
        master_ = ::getppid();
        install(app);
        loop.add_timer(30s, [this] { fail("the scenario did not finish in 30 s"); }, false);
        after(1ms, [this] { g_on ? step_collect() : step_off(); });
    }

private:
    Application* app_{nullptr};
    EventLoop*   loop_{nullptr};
    pid_t        master_{0};
    bool         finished_{false};

    Ipc& ipc() { return app_->ipc(); }

    void fail(const std::string& why)
    {
        if (finished_)
            return;
        finished_ = true;
        app_->logger().error("probe: FAIL {}", why);
        write_file(result_file(), "FAIL " + why + "\n");
        ::kill(master_, SIGTERM);
    }

    void pass()
    {
        if (finished_)
            return;
        finished_ = true;
        app_->logger().notice("probe: PASS");
        write_file(result_file(), "PASS\n");
        ::kill(master_, SIGTERM);
    }

    void after(std::chrono::milliseconds d, std::function<void()> fn)
    {
        loop_->add_timer(d, [this, fn = std::move(fn)] { if (!finished_) fn(); }, false);
    }

    void wait_until(std::function<bool()> pred, Clock::time_point deadline,
                    std::string what, std::function<void()> next)
    {
        if (finished_)
            return;
        if (pred())
            return next();
        if (Clock::now() > deadline)
            return fail("timed out waiting for " + what);
        after(50ms, [this, pred = std::move(pred), deadline, what = std::move(what),
                     next = std::move(next)]() mutable {
            wait_until(std::move(pred), deadline, std::move(what), std::move(next));
        });
    }

    // One TYPE per family, each family's lines in one run. Empty — fine.
    static std::string well_formed(const std::vector<std::string>& lines)
    {
        std::map<std::string, int> types;
        std::set<std::string>      closed;
        std::string                current;
        for (const auto& l : lines) {
            if (l.rfind("# TYPE ", 0) == 0)
                ++types[family_of_line(l)];
            auto f = family_of_line(l);
            for (std::string_view s : {"_bucket", "_sum", "_count"})
                if (f.size() > s.size() && f.ends_with(s) && types.contains(f.substr(0, f.size() - s.size())))
                    f = f.substr(0, f.size() - s.size());
            if (f != current) {
                if (closed.contains(f))
                    return "family " + f + " is split";
                if (!current.empty())
                    closed.insert(current);
                current = f;
            }
        }
        for (const auto& [f, n] : types)
            if (n != 1)
                return fmt::format("family {} has {} TYPE lines", f, n);
        return {};
    }

    static std::size_t count(const std::vector<std::string>& lines, std::string_view part)
    {
        std::size_t n = 0;
        for (const auto& l : lines)
            if (l.find(part) != std::string::npos)
                ++n;
        return n;
    }

    // ── on ───────────────────────────────────────────────────────────────────

    void step_collect()
    {
        wait_until([this] { return ipc().peers(ipc_all).size() == kAll; }, Clock::now() + 10s,
                   "every process live", [this] {
            app_->collect_metrics([this](std::string) {
                // The second one: the first collection is counted by now.
                app_->collect_metrics([this](std::string text) { check_full(text); }, 2s);
            }, 2s);
        });
    }

    void check_full(const std::string& text)
    {
        const auto lines = lines_of(text);
        if (auto bad = well_formed(lines); !bad.empty())
            return fail("collection: " + bad + "\n" + text);

        const std::vector<std::string> up = {
            R"(t_process_up{role="worker",worker="1"} 1)",
            R"(t_process_up{role="worker",worker="2"} 1)",
            R"(t_process_up{role="helper",worker="0"} 1)",
            R"(t_process_up{role="probe",worker="0"} 1)",
        };
        for (const auto& u : up)
            if (count(lines, u) != 1)
                return fail("collection: no single '" + u + "'\n" + text);
        if (count(lines, "t_process_up{") != kAll)
            return fail("collection: process_up rows other than one per process\n" + text);

        for (int n = 1; n <= kWorkers; ++n)
            if (count(lines, fmt::format(R"(t_test_rows{{role="worker",worker="{}"}} 7)", n)) != 1)
                return fail(fmt::format("collection: no own row of worker {}\n{}", n, text));
        if (count(lines, "t_test_rows{") != kWorkers)
            return fail("collection: own rows not from the workers alone\n" + text);

        for (std::string_view role : {R"(role="worker",worker="1")", R"(role="worker",worker="2")",
                                      R"(role="helper",worker="0")", R"(role="probe",worker="0")"})
            if (count(lines, fmt::format(R"(t_ipc_messages_total{{{},type="metrics",result="ok"}})", role)) != 1)
                return fail(fmt::format("collection: no channel counter of {}\n{}", role, text));

        app_->logger().notice("probe: 1. a collection answers for every process ({} lines)", lines.size());
        step_stalled();
    }

    void step_stalled()
    {
        auto helpers = ipc().peers(ipc_helper);
        if (helpers.size() != 1)
            return fail("no helper");
        if (!ipc().send(helpers.front().slot, "stall", "1500"))
            return fail("stall: not sent");
        after(100ms, [this] {
            app_->collect_metrics([this](std::string text) {
                const auto lines = lines_of(text);
                if (auto bad = well_formed(lines); !bad.empty())
                    return fail("stalled collection: " + bad + "\n" + text);
                if (count(lines, R"(t_process_up{role="helper",worker="0"} 0)") != 1)
                    return fail("stalled collection: the helper is not 0\n" + text);
                if (count(lines, R"(role="helper")") != 1)
                    return fail("stalled collection: rows of the helper besides process_up\n" + text);
                if (count(lines, "t_process_up{") != kAll || count(lines, "} 1") < kAll - 1)
                    return fail("stalled collection: the others are not all 1\n" + text);
                app_->logger().notice("probe: 2. a process without an answer is 0, its rows left out");
                after(1800ms, [this] { step_back(); });
            }, 400ms);
        });
    }

    void step_back()
    {
        app_->collect_metrics([this](std::string text) {
            const auto lines = lines_of(text);
            if (count(lines, R"(t_process_up{role="helper",worker="0"} 1)") != 1)
                return fail("after the stall: the helper is not 1\n" + text);
            if (count(lines, R"(role="helper")") < 2)
                return fail("after the stall: the helper's rows are not back\n" + text);
            pass();
        }, 2s);
    }

    // ── off ──────────────────────────────────────────────────────────────────

    void step_off()
    {
        // The children appear one by one: wait for all of them.
        wait_until([this] { return children_of(master_).size() == kAll; }, Clock::now() + 10s,
                   "every process forked", [this] {
            if (ipc().connected())
                return fail("off: connected");
            if (!ipc().peers(ipc_all).empty() && ipc().peers(ipc_all).size() != 1)
                return fail(fmt::format("off: {} peers", ipc().peers(ipc_all).size()));
            if (ipc().send(0, "who", {}) || ipc().send(1, "who", {}))
                return fail("off: a send was taken");

            // What each process was started with, and the listening socket.
            const int want = g_base_sockets + 1;
            if (const int n = sockets_of(master_); n != want)
                return fail(fmt::format("off: master holds {} sockets, not {}", n, want));
            for (pid_t pid : children_of(master_))
                if (const int n = sockets_of(pid); n != want)
                    return fail(fmt::format("off: process {} holds {} sockets, not {}", pid, n, want));

            ipc().gather(ipc_all, "who", {}, [this](IpcGathered g) {
                if (g.replies.size() != 1 || !g.missing.empty() || g.replies.front().second != "probe")
                    return fail(fmt::format("off: gather {} replies, {} missing", g.replies.size(),
                                            g.missing.size()));
                app_->collect_metrics([this](std::string text) {
                    if (!text.empty())
                        return fail("off: a collection gave rows\n" + text);
                    if (!app_->process_metrics().empty())
                        return fail("off: the process gave rows");
                    app_->logger().notice("probe: off — no channels, the process alone, no rows");
                    pass();
                }, 1s);
            }, 1s);
        });
    }
};

// ─── The application ─────────────────────────────────────────────────────────

class TestApp final : public Application
{
public:
    TestApp() : Application("metrics_test")
    {
        if (g_on)
            enable_metrics("t");
    }

protected:
    void on_worker_start(EventLoop&) override
    {
        install(*this);
        add_metrics([](MetricsText& m) { m.gauge("test_rows", "A row of the worker's own.", 7); });
    }

    void on_helper_start(EventLoop&) override { install(*this); }

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

} // namespace

int main(int argc, char* argv[])
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "-p")
            g_dir = argv[i + 1];
        else if (std::string_view(argv[i]) == "--mode")
            g_on = std::string_view(argv[i + 1]) != "off";
    }
    if (g_dir.empty()) {
        std::fprintf(stderr, "usage: metrics_test --mode on|off -p <dir>/ -c <dir>/metrics_test.json\n");
        return 2;
    }

    if (auto bad = check_text(); !bad.empty()) {
        std::printf("metrics_test: FAIL %s\n", bad.c_str());
        return 1;
    }

    g_base_sockets = sockets_of(::getpid());

    // The master always binds a listener: a port nobody holds right now.
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
        std::fprintf(stderr, "metrics_test: no free port\n");
        return 2;
    }

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "logs");
    write_file(g_dir / "metrics_test.json", fmt::format(R"({{
  "workers": {},
  "process": {{ "master": true, "helper": true, "stop_timeout": 5 }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice" }}
}}
)", kWorkers, port));

    // The application reads the command line too: without "--mode <x>".
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--mode" && i + 1 < argc) {
            ++i;
            continue;
        }
        args.push_back(argv[i]);
    }
    args.push_back(nullptr);

    int rc = 0;
    {
        TestApp app;
        rc = app.run(static_cast<int>(args.size()) - 1, args.data());
    }

    const auto verdict = read_file(result_file());
    std::printf("metrics_test %s: %s", g_on ? "on" : "off",
                verdict.empty() ? "no verdict — the probe did not finish\n" : verdict.c_str());
    std::printf("metrics_test: master exit %d; log %s\n", rc, (g_dir / "logs/error.log").c_str());
    return rc == 0 && verdict.rfind("PASS", 0) == 0 ? 0 : 1;
}
