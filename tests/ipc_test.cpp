// ipc_test — the channels between the processes of one node, end to end.
//
// The test is an application: a master with three workers, the helper and a
// custom process "probe", no database and no HTTP. Every process answers the
// same handful of message types; the probe drives the scenario and writes its
// verdict into <dir>/result, then stops the master. main() returns 0 only for
// a clean master exit with the verdict PASS.
//
//   1. every process is live and a gather reaches all of them;
//   2. a request and its answer survive being cut into chunks;
//   3. a process's first gather, sent before its neighbours were told about
//      it, is answered by all of them — forced once: the helper is held inside
//      its inbox when the respawned worker asks, so it answers before it has
//      read the master's "open" and must keep the answer until then;
//   4. two writers push 4 MiB each into a receiver busy elsewhere: both arrive
//      whole, and both writers did find the channel full;
//   5. every process holds exactly its own inbox, its control end and one end
//      per neighbour; the master two per child (and each the listening socket);
//   6. a worker killed with a request in flight: the sender gets "gone", not a
//      timeout; the respawned worker takes a slot and answers again, and no
//      process keeps the dead one's channel;
//   7. a reload (SIGHUP): while the old generation is still stopping, a gather
//      answers once per worker number, and the new processes' first gathers
//      are answered by everyone.
//
// Usage: ipc_test -p <dir>/ -c <dir>/ipc_test.json   (ctest passes both)

#include "apostol/application.hpp"
#include "apostol/http.hpp"
#include "apostol/ipc.hpp"
#include "apostol/module.hpp"
#include "apostol/process_module.hpp"

#include <fmt/format.h>

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
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

constexpr std::size_t kBig      = 4 * 1024 * 1024;   // well over SO_SNDBUF
constexpr std::size_t kChunked  = 100 * 1024;        // a few chunks
constexpr int         kWorkers  = 3;
constexpr int         kAll      = kWorkers + 2;      // + helper + probe

fs::path g_dir;
int      g_base_sockets = 0;   // what main() was started with (ctest's stdio may be sockets)

fs::path result_file() { return g_dir / "result"; }
fs::path stage_file()  { return g_dir / "stage"; }

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

// A body only its own seed produces: a lost, doubled or misplaced chunk shows.
std::string blob(std::uint32_t seed, std::size_t n)
{
    std::string b(std::max<std::size_t>(n, 4), '\0');
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::size_t i = 4; i < b.size(); ++i)
    {
        x = x * 1664525u + 1013904223u;
        b[i] = static_cast<char>(x >> 24);
    }
    std::memcpy(b.data(), &seed, 4);
    return b;
}

bool blob_ok(std::string_view b)
{
    if (b.size() < 4)
        return false;
    std::uint32_t seed = 0;
    std::memcpy(&seed, b.data(), 4);
    return b == blob(seed, b.size());
}

int sockets_of(pid_t pid)
{
    std::error_code ec;
    fs::directory_iterator it(fmt::format("/proc/{}/fd", pid), ec), end;
    if (ec)
        return -1;
    int n = 0;
    for (; it != end; it.increment(ec))
    {
        if (ec)
            return -1;
        std::error_code e2;
        const auto target = fs::read_symlink(it->path(), e2).string();
        if (!e2 && target.rfind("socket:", 0) == 0)
            ++n;
    }
    return n;
}

bool alive(pid_t pid)
{
    const auto stat = read_file(fmt::format("/proc/{}/stat", pid));
    const auto close = stat.rfind(')');
    return close != std::string::npos && close + 2 < stat.size() && stat[close + 2] != 'Z';
}

// ─── What every process answers ──────────────────────────────────────────────

std::vector<IpcReply> g_hung;   // "hang": kept, never answered

struct FirstGather
{
    bool        done{false};
    std::string summary;        // "<replies> <missing>[ <name>:<error>…]"
    std::vector<std::shared_ptr<IpcReply>> waiting;
} g_first;

void install(Application& app, EventLoop& loop)
{
    auto& ipc = app.ipc();

    ipc.on("who", [&app](const IpcPeer&, std::string_view, IpcReply r) {
        const auto me = app.ipc().self();
        r.send(fmt::format("{} {} {}", me.name, me.number, me.pid));
    });
    ipc.on("echo", [](const IpcPeer&, std::string_view b, IpcReply r) {
        r.send(std::string(b));
    });
    ipc.on("held", [&app](const IpcPeer&, std::string_view, IpcReply r) {
        r.send(std::to_string(app.ipc().stats().held));
    });
    ipc.on("sockets", [](const IpcPeer&, std::string_view, IpcReply r) {
        r.send(std::to_string(sockets_of(::getpid())));
    });
    ipc.on("hang", [](const IpcPeer&, std::string_view, IpcReply r) {
        g_hung.push_back(std::move(r));
    });
    // Busy elsewhere: the loop is held, nothing is read meanwhile.
    ipc.on("stall", [](const IpcPeer&, std::string_view b, IpcReply) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(std::string(b))));
    });
    ipc.on("blob", [](const IpcPeer&, std::string_view b, IpcReply r) {
        if (blob_ok(b))
            r.send(fmt::format("ok {}", b.size()));
        else
            r.fail();
    });
    // "<slot> <seed> <size>": send a blob there, answer how it went and how
    // many times this process found a channel full meanwhile.
    ipc.on("push", [&app](const IpcPeer&, std::string_view b, IpcReply r) {
        std::istringstream in{std::string(b)};
        int slot = -1;
        std::uint32_t seed = 0;
        std::size_t size = 0;
        in >> slot >> seed >> size;
        auto reply = std::make_shared<IpcReply>(std::move(r));
        const auto blocked = app.ipc().stats().blocked;
        app.ipc().request(slot, "blob", blob(seed, size), [&app, reply, blocked](IpcResult res) {
            reply->send(fmt::format("{} {} {}", ipc_error_name(res.error),
                                    app.ipc().stats().blocked - blocked, res.body));
        }, 30s);
    });
    ipc.on("first", [](const IpcPeer&, std::string_view, IpcReply r) {
        if (g_first.done)
            r.send(g_first.summary);
        else
            g_first.waiting.push_back(std::make_shared<IpcReply>(std::move(r)));
    });

    // The first gather, as soon as the loop runs — before the neighbours may
    // have been told about this process.
    loop.add_timer(1ms, [&app] {
        app.ipc().gather(ipc_all, "who", {}, [](IpcGathered g) {
            std::string s = fmt::format("{} {}", g.replies.size(), g.missing.size());
            for (const auto& [peer, e] : g.missing)
                s += fmt::format(" {}:{}", peer.name, ipc_error_name(e));
            g_first.summary = std::move(s);
            g_first.done = true;
            for (auto& r : g_first.waiting)
                r->send(g_first.summary);
            g_first.waiting.clear();
        }, 5s);
    }, /*repeat=*/false);
}

// A worker that takes a while to stop: the old generation of a reload stays
// on the node, retiring, while the new one answers.
class SlowStop final : public Module
{
public:
    std::string_view name() const override { return "SlowStop"; }
    bool enabled() const override { return true; }
    bool execute(const HttpRequest&, HttpResponse&) override { return false; }
    void on_stopping() override { since_ = Clock::now(); }
    bool busy() const override
    {
        return since_ != Clock::time_point{} && Clock::now() - since_ < 1500ms;
    }

private:
    Clock::time_point since_{};
};

// ─── The probe ───────────────────────────────────────────────────────────────

class Probe final : public ProcessModule
{
public:
    std::string_view name() const override { return "probe"; }

    void on_start(EventLoop& loop, Application& app) override
    {
        app_    = &app;
        loop_   = &loop;
        master_ = ::getppid();
        install(app, loop);
        loop.add_timer(60s, [this] { fail("the scenario did not finish in 60 s"); }, false);

        const auto stage = read_file(stage_file());
        if (stage.rfind("hup", 0) == 0)
            after(1ms, [this, stage] { after_reload(stage); });
        else
            after(1ms, [this] { step_live(); });
    }

private:
    Application* app_{nullptr};
    EventLoop*   loop_{nullptr};
    pid_t        master_{0};
    bool         finished_{false};
    std::map<int, IpcPeer> workers_;   // number → peer

    Ipc& ipc() { return app_->ipc(); }

    void note(std::string_view s) { app_->logger().notice("probe: {}", s); }

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
        note("PASS");
        write_file(result_file(), "PASS\n");
        ::kill(master_, SIGTERM);
    }

    bool expect(bool ok, const std::string& what)
    {
        if (!ok)
            fail(what);
        return ok && !finished_;
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

    bool all_live()
    {
        return ipc().peers(ipc_all).size() == kAll
            && ipc().peers(ipc_workers).size() == kWorkers
            && ipc().peers(ipc_helper).size() == 1;
    }

    void remember_workers()
    {
        workers_.clear();
        for (auto& p : ipc().peers(ipc_workers))
            workers_[p.number] = p;
    }

    // ── 1. everyone is there, a gather reaches all ───────────────────────────

    void step_live()
    {
        wait_until([this] { return all_live(); }, Clock::now() + 10s,
                   "three workers, the helper and the probe live", [this] {
            remember_workers();
            if (!expect(workers_.size() == kWorkers && workers_.count(1) && workers_.count(2)
                            && workers_.count(3),
                        "worker numbers are not 1, 2, 3"))
                return;
            note("all live");
            ipc().gather(ipc_all, "who", {}, [this](IpcGathered g) {
                if (!expect(g.replies.size() == kAll && g.missing.empty(),
                            fmt::format("gather: {} replies, {} missing", g.replies.size(),
                                        g.missing.size())))
                    return;
                std::multiset<int> numbers;
                for (const auto& [peer, body] : g.replies)
                    if (peer.role == ProcessRole::worker)
                        numbers.insert(peer.number);
                if (!expect(numbers == std::multiset<int>{1, 2, 3}, "gather: worker numbers"))
                    return;
                note("gather reached all");
                step_echo();
            }, 3s);
        });
    }

    // ── 2. chunked request and answer ────────────────────────────────────────

    void step_echo()
    {
        auto left = std::make_shared<int>(kWorkers);
        for (const auto& [n, peer] : workers_)
        {
            auto body = blob(100 + static_cast<std::uint32_t>(n), kChunked);
            ipc().request(peer.slot, "echo", body, [this, left, body](IpcResult r) {
                if (!expect(r.ok() && r.body == body,
                            fmt::format("echo: {} ({} bytes back)", ipc_error_name(r.error),
                                        r.body.size())))
                    return;
                if (--*left == 0)
                {
                    note("chunked echo ok");
                    step_first();
                }
            }, 3s);
        }
    }

    // ── 3. first gathers were answered by everyone ───────────────────────────

    void check_first(const std::vector<IpcPeer>& targets, std::size_t replies_expected,
                     std::function<void()> next)
    {
        auto left = std::make_shared<std::size_t>(targets.size());
        for (const auto& peer : targets)
        {
            ipc().request(peer.slot, "first", {},
                          [this, left, peer, replies_expected, next](IpcResult r) {
                if (!expect(r.ok(), fmt::format("first from {}: {}", peer.name,
                                                ipc_error_name(r.error))))
                    return;
                std::istringstream in(r.body);
                std::size_t replies = 0, missing = 99;
                in >> replies >> missing;
                if (!expect(missing == 0 && replies >= 1
                                && (replies_expected == 0 || replies == replies_expected),
                            fmt::format("first gather of {}: '{}'", peer.name, r.body)))
                    return;
                if (--*left == 0)
                    next();
            }, 6s);
        }
    }

    void step_first()
    {
        auto targets = ipc().peers(ipc_workers | ipc_helper);
        check_first(targets, 0, [this] {
            note("first gathers answered by all");
            step_eagain();
        });
    }

    // ── 4. two writers, 4 MiB each, a busy receiver ──────────────────────────

    void step_eagain()
    {
        const auto& w1 = workers_[1];
        const auto& w2 = workers_[2];
        const auto& w3 = workers_[3];
        // Ahead of both pushes in w3's inbox: it sleeps while they write.
        if (!expect(ipc().send(w3.slot, "stall", "700"), "stall: not sent"))
            return;
        auto left = std::make_shared<int>(2);
        for (const auto* w : {&w1, &w2})
        {
            const auto seed = static_cast<std::uint32_t>(w->number);
            ipc().request(w->slot, "push", fmt::format("{} {} {}", w3.slot, seed, kBig),
                          [this, left, name = w->name](IpcResult r) {
                if (!expect(r.ok(), fmt::format("push via {}: {}", name, ipc_error_name(r.error))))
                    return;
                std::istringstream in(r.body);
                std::string error, ok;
                unsigned long long blocked = 0, size = 0;
                in >> error >> blocked >> ok >> size;
                if (!expect(error == "ok" && ok == "ok" && size == kBig,
                            fmt::format("push via {}: '{}'", name, r.body)))
                    return;
                if (!expect(blocked > 0, fmt::format("push via {}: the channel was never full — "
                                                     "the queue was not exercised", name)))
                    return;
                note(fmt::format("4 MiB via {} whole, {} waits for room", name, blocked));
                if (--*left == 0)
                    step_sockets(10, [this] { step_kill(); });
            }, 30s);
        }
    }

    // ── 5. descriptors: own inbox + control + one per neighbour ──────────────

    void step_sockets(int attempts, std::function<void()> next)
    {
        ipc().gather(ipc_all, "sockets", {}, [this, attempts, next](IpcGathered g) {
            std::string bad;
            if (g.replies.size() != kAll || !g.missing.empty())
                bad += fmt::format(" gather {}/{}", g.replies.size(), g.missing.size());
            // + 1: the master's listening socket, inherited by every child;
            // + what the test was started with, inherited by everyone too.
            const int base = g_base_sockets + 1;
            for (const auto& [peer, body] : g.replies)
                if (body != std::to_string(base + 2 + (kAll - 1)))
                    bad += fmt::format(" {}={}", peer.name, body);
            const int master = sockets_of(master_);
            if (master != base + 2 * kAll)
                bad += fmt::format(" master={}", master);
            if (bad.empty())
            {
                note("sockets as expected");
                return next();
            }
            if (attempts <= 1)
                return fail("sockets:" + bad);
            after(200ms, [this, attempts, next] { step_sockets(attempts - 1, next); });
        }, 3s);
    }

    // ── 6. a worker killed with a request in flight ──────────────────────────

    void step_kill()
    {
        const IpcPeer victim = workers_[2];
        const auto helpers = ipc().peers(ipc_helper);
        if (!expect(helpers.size() == 1, "kill: no helper"))
            return;
        // The helper sleeps inside its inbox while the worker is respawned and
        // sends its first gather; waking, it reads the rest of the inbox — that
        // request — before the master's "open" for the new worker.
        if (!expect(ipc().send(helpers.front().slot, "stall", "1500"), "stall: not sent"))
            return;
        const auto t0 = Clock::now();
        ipc().request(victim.slot, "hang", {}, [this, victim, t0](IpcResult r) {
            const auto took = Clock::now() - t0;
            if (!expect(r.error == IpcError::gone,
                        fmt::format("request to a killed worker: {}, not gone",
                                    ipc_error_name(r.error))))
                return;
            if (!expect(took < 5s, "request to a killed worker: answered too late"))
                return;
            note(fmt::format("killed worker: gone after {} ms",
                             std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
            step_respawned(victim);
        }, 15s);
        after(300ms, [victim] { ::kill(victim.pid, SIGKILL); });
    }

    void step_respawned(IpcPeer victim)
    {
        wait_until([this, victim] {
            if (!all_live())
                return false;
            for (const auto& p : ipc().peers(ipc_workers))
                if (p.number == victim.number && p.pid != victim.pid)
                    return true;
            return false;
        }, Clock::now() + 10s, "the killed worker to be respawned", [this, victim] {
            remember_workers();
            const IpcPeer fresh = workers_[victim.number];
            if (!expect(fresh.generation != victim.generation, "respawn: same generation"))
                return;
            ipc().request(fresh.slot, "who", {}, [this, fresh](IpcResult r) {
                if (!expect(r.ok() && r.body.find(std::to_string(fresh.pid)) != std::string::npos,
                            fmt::format("respawned worker: {} '{}'", ipc_error_name(r.error), r.body)))
                    return;
                note(fmt::format("respawned {} (slot {}) answers", fresh.name, fresh.slot));
                check_first({fresh}, kAll, [this] {
                    note("its first gather answered by all");
                    const auto helper = ipc().peers(ipc_helper).front();
                    ipc().request(helper.slot, "held", {}, [this](IpcResult r) {
                        if (!expect(r.ok() && r.body != "0",
                                    fmt::format("the helper kept no answer for the channel to "
                                                "open ({} '{}'): the case was not forced",
                                                ipc_error_name(r.error), r.body)))
                            return;
                        note(fmt::format("the helper kept {} message(s) until 'open'", r.body));
                        step_sockets(15, [this] { step_reload(); });
                    }, 3s);
                });
            }, 3s);
        });
    }

    // ── 7. reload ────────────────────────────────────────────────────────────

    void step_reload()
    {
        std::string stage = "hup";
        for (const auto& p : ipc().peers(ipc_workers))
            stage += fmt::format(" {}", p.pid);
        write_file(stage_file(), stage + "\n");
        note("SIGHUP to the master");
        ::kill(master_, SIGHUP);
        // This probe is of the old generation now: the new one goes on.
    }

    void after_reload(const std::string& stage)
    {
        std::istringstream in(stage);
        std::string word;
        in >> word;
        auto old = std::make_shared<std::vector<pid_t>>();
        for (pid_t pid; in >> pid;)
            old->push_back(pid);
        note("after the reload");

        wait_until([this, old] {
            if (!all_live())
                return false;
            for (const auto& p : ipc().peers(ipc_workers))
                for (auto o : *old)
                    if (p.pid == o)
                        return false;
            return true;
        }, Clock::now() + 10s, "the new generation live", [this, old] {
            int window = 0;
            for (auto o : *old)
                window += alive(o) ? 1 : 0;
            ipc().gather(ipc_all, "who", {}, [this, old, window](IpcGathered g) {
                if (!expect(g.replies.size() == kAll && g.missing.empty(),
                            fmt::format("reload gather: {} replies, {} missing",
                                        g.replies.size(), g.missing.size())))
                    return;
                std::multiset<int> numbers;
                for (const auto& [peer, body] : g.replies)
                {
                    if (peer.role != ProcessRole::worker)
                        continue;
                    numbers.insert(peer.number);
                    for (auto o : *old)
                        if (peer.pid == o)
                            return fail("reload gather: an old worker answered");
                }
                if (!expect(numbers == std::multiset<int>{1, 2, 3},
                            "reload gather: not one answer per worker number"))
                    return;
                if (!expect(window > 0, "reload: no old worker was still stopping — the window "
                                        "of two generations was not observed"))
                    return;
                note(fmt::format("reload gather: one per number, {} old still stopping", window));
                check_first(ipc().peers(ipc_workers | ipc_helper), 0, [this] {
                    note("new generation's first gathers answered by all");
                    pass();
                });
            }, 3s);
        });
    }
};

// ─── The application ─────────────────────────────────────────────────────────

class TestApp final : public Application
{
public:
    TestApp() : Application("ipc_test") { enable_ipc(); }

protected:
    void on_worker_start(EventLoop& loop) override
    {
        module_manager().add_module(std::make_unique<SlowStop>());
        install(*this, loop);
    }

    void on_helper_start(EventLoop& loop) override { install(*this, loop); }

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
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "-p")
            g_dir = argv[i + 1];
    if (g_dir.empty())
    {
        std::fprintf(stderr, "usage: ipc_test -p <dir>/ -c <dir>/ipc_test.json\n");
        return 2;
    }

    g_base_sockets = sockets_of(::getpid());

    // The master always binds a listener: a port nobody holds right now.
    int port = 0;
    if (int fd = ::socket(AF_INET, SOCK_STREAM, 0); fd >= 0)
    {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len     = sizeof(a);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0
            && ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
            port = ntohs(a.sin_port);
        ::close(fd);
    }
    if (port == 0)
    {
        std::fprintf(stderr, "ipc_test: no free port\n");
        return 2;
    }

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "logs");
    write_file(g_dir / "ipc_test.json", fmt::format(R"({{
  "workers": 3,
  "process": {{ "master": true, "helper": true, "stop_timeout": 5 }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice" }}
}}
)", port));

    int rc = 0;
    {
        TestApp app;
        rc = app.run(argc, argv);
    }

    const auto verdict = read_file(result_file());
    std::printf("ipc_test: %s", verdict.empty() ? "no verdict — the probe did not finish\n"
                                                : verdict.c_str());
    std::printf("ipc_test: master exit %d; log %s\n", rc, (g_dir / "logs/error.log").c_str());
    return rc == 0 && verdict.rfind("PASS", 0) == 0 ? 0 : 1;
}
