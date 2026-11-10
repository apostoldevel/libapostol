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
//      process keeps the dead one's channel. With the master stopped, the dead
//      worker stays in the directory: a request to it finds its channel dead
//      while being sent, and no callback runs before request() returns;
//   7. a request sent to the respawned worker while it is still starting —
//      held back before it attaches its channels — waits for its handlers
//      instead of being refused as "no_handler";
//   8. answers owed by a stopping process: two workers get SIGTERM while each
//      holds a request of the probe. The one that answers 300 ms later — its
//      stop already begun — still delivers the answer; the one that never
//      answers stops once the probe has stopped waiting (its 700 ms), not
//      sooner and not at the end of the stop's budget. Both are respawned.
//      Then a worker asks another one (never answered, 5 s) and is stopped
//      first, kept on the node by an answer it owes the probe: the other,
//      stopped next, owes nothing to an asker that has stopped waiting and
//      exits at once, not at the end of its budget;
//   9. a reload (SIGHUP): while the old generation is still stopping, a gather
//      answers once per worker number, and the new processes' first gathers
//      are answered by everyone; each old worker, stopping with a request in
//      flight since its start whose callback asks again, saw both callbacks
//      run as "stopping", each with ipc().stopping() true (false in step 1).
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
fs::path slow_file()   { return g_dir / "slowstart"; }        // the next worker sleeps before it starts
fs::path woke_file()   { return g_dir / "slowstart.woke"; }   // when it woke
fs::path detach_file(pid_t pid) { return g_dir / fmt::format("detach.{}", pid); }

// CLOCK_MONOTONIC: the same clock in every process of the host.
long long mono_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

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
    // "<slot> <ms>": asks that worker "hang" for that long; answered at once.
    ipc.on("ask", [&ipc](const IpcPeer&, std::string_view b, IpcReply r) {
        const std::string body(b);
        const auto sp = body.find(' ');
        ipc.request(std::stoi(body.substr(0, sp)), "hang", {}, [](IpcResult) {},
                    std::chrono::milliseconds(std::stoi(body.substr(sp + 1))));
        r.send("asked");
    });
    // "<ms>": answered that much later, from the loop — a stop begun meanwhile
    // included.
    ipc.on("later", [&loop](const IpcPeer&, std::string_view b, IpcReply r) {
        auto reply = std::make_shared<IpcReply>(std::move(r));
        loop.add_timer(std::chrono::milliseconds(std::stoi(std::string(b))), [reply] {
            reply->send(fmt::format("later {}", ::getpid()));
        }, /*repeat=*/false);
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

// A worker that takes a while to stop in a reload: the old generation stays
// on the node, retiring, while the new one answers. (Only then: in step 8 the
// stop must wait for nothing but the answers owed.) From its start it keeps a
// request in flight (to itself, never answered) whose callback asks again:
// when the worker stops, both callbacks must run as its stop begins — the
// verdict goes to detach.<pid>. (Not from on_stopping(): that runs after the
// stop has begun, when a request fails at once.)
class SlowStop final : public Module
{
public:
    explicit SlowStop(Application& app) : app_(app) {}

    std::string_view name() const override { return "SlowStop"; }
    bool enabled() const override { return true; }
    bool execute(const HttpRequest&, HttpResponse&) override { return false; }
    void on_start() override
    {
        Application* app = &app_;
        const int self = app->ipc().self().slot;
        app->ipc().request(self, "hang", {}, [app, self](IpcResult first) {
            const bool first_stopping = app->ipc().stopping();
            app->ipc().request(self, "who", {}, [app, first = first.error, first_stopping](IpcResult second) {
                write_file(detach_file(::getpid()),
                           fmt::format("{} {} {} {}\n", ipc_error_name(first), ipc_error_name(second.error),
                                       first_stopping ? "stopping()" : "-",
                                       app->ipc().stopping() ? "stopping()" : "-"));
            }, 1s);
        }, 600s);
    }
    void on_stopping() override
    {
        if (read_file(stage_file()).rfind("hup", 0) == 0)
            since_ = Clock::now();
    }
    bool busy() const override
    {
        return since_ != Clock::time_point{} && Clock::now() - since_ < 1500ms;
    }

private:
    Application&      app_;
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
            if (!expect(!ipc().stopping(), "stopping() is true in a live process"))
                return;
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
        auto inside = std::make_shared<bool>(false);   // inside a call of request()
        ipc().request(victim.slot, "hang", {}, [this, victim, t0, inside](IpcResult r) {
            const auto took = Clock::now() - t0;
            if (!expect(!*inside, "kill: a callback ran inside request()"))
                return;
            if (!expect(r.error == IpcError::gone,
                        fmt::format("request to a killed worker: {}, not gone",
                                    ipc_error_name(r.error))))
                return;
            if (!expect(took < 5s, "request to a killed worker: answered too late"))
                return;
            note(fmt::format("killed worker: gone after {} ms",
                             std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
            step_starting(victim, Clock::now() + 10s, [this, victim] { step_respawned(victim); });
        }, 15s);
        after(300ms, [this, victim, inside] {
            // The respawned worker sleeps before it starts (step 7).
            write_file(slow_file(), "1\n");
            // The master stopped: it does not reap, the dead worker stays in the
            // directory and its channel here stays open — the next write to it
            // fails inside request() itself.
            ::kill(master_, SIGSTOP);
            ::kill(victim.pid, SIGKILL);
            const auto until = Clock::now() + 2s;
            while (alive(victim.pid) && Clock::now() < until)
                std::this_thread::sleep_for(1ms);
            const bool dead = !alive(victim.pid);
            *inside = true;
            ipc().request(victim.slot, "who", {}, [this, inside](IpcResult r) {
                if (!expect(!*inside, "kill: request() called back before it returned"))
                    return;
                expect(r.error == IpcError::gone,
                       fmt::format("request to a dead worker: {}, not gone", ipc_error_name(r.error)));
            }, 3s);
            *inside = false;
            ::kill(master_, SIGCONT);
            expect(dead, "kill: the worker did not die in 2 s");
        });
    }

    // ── 7. a request to a worker that has not started yet ────────────────────

    void step_starting(IpcPeer victim, Clock::time_point deadline, std::function<void()> next)
    {
        // The old process in the slot answers "gone" (or never got the
        // request); the new one takes the same slot — the first free.
        const long long sent = mono_ms();
        ipc().request(victim.slot, "who", {}, [this, victim, deadline, next, sent](IpcResult r) {
            if (r.error == IpcError::gone || r.error == IpcError::unreachable)
            {
                if (Clock::now() > deadline)
                    return fail("starting: no new process in the slot of the killed worker");
                return after(2ms, [this, victim, deadline, next] { step_starting(victim, deadline, next); });
            }
            if (!expect(r.ok() && r.body.find(std::to_string(victim.pid)) == std::string::npos,
                        fmt::format("request to a starting worker: {} '{}'", ipc_error_name(r.error),
                                    r.body)))
                return;
            const auto woke = read_file(woke_file());
            if (!expect(!woke.empty() && sent < std::stoll(woke),
                        fmt::format("starting: the request (at {}) was not sent before the worker "
                                    "started (at '{}') — the case was not forced", sent, woke)))
                return;
            note(fmt::format("a request sent {} ms before the worker started was answered",
                             std::stoll(woke) - sent));
            next();
        }, 5s);
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
                        step_sockets(15, [this] { step_owed(); });
                    }, 3s);
                });
            }, 3s);
        });
    }

    // ── 8. answers owed by a stopping process ───────────────────────────────

    void step_owed()
    {
        const IpcPeer late = workers_[3];   // answers 300 ms after the request
        const IpcPeer mute = workers_[1];   // never answers; the probe waits 700 ms
        const auto t0 = Clock::now();
        auto left = std::make_shared<int>(4);   // two answers, two exits
        auto term = std::make_shared<Clock::time_point>();
        auto next = [this, left, late, mute] {
            if (--*left == 0)
                step_owed_respawned(late, mute, [this] {
                    step_sockets(15, [this] { step_owed_asker(); });
                });
        };
        auto ms = [](Clock::duration d) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
        };

        ipc().request(late.slot, "later", "300", [this, late, t0, next, ms](IpcResult r) {
            if (!expect(r.ok() && r.body == fmt::format("later {}", late.pid),
                        fmt::format("owed: the answer of {}, stopping, did not arrive: {} '{}'",
                                    late.name, ipc_error_name(r.error), r.body)))
                return;
            note(fmt::format("owed: {} answered {} ms after the request, while stopping",
                             late.name, ms(Clock::now() - t0)));
            next();
        }, 5s);
        ipc().request(mute.slot, "hang", {}, [this, mute, t0, next, ms](IpcResult r) {
            if (!expect(r.error == IpcError::timeout,
                        fmt::format("owed: {} kept the answer: {}, not timeout", mute.name,
                                    ipc_error_name(r.error))))
                return;
            note(fmt::format("owed: {} timed out after {} ms", mute.name, ms(Clock::now() - t0)));
            next();
        }, 700ms);

        after(50ms, [this, late, mute, term, next, ms] {
            *term = Clock::now();
            ::kill(late.pid, SIGTERM);
            ::kill(mute.pid, SIGTERM);
            for (const IpcPeer& w : {late, mute})
                wait_until([pid = w.pid] { return !alive(pid); }, Clock::now() + 10s,
                           w.name + " to exit after SIGTERM", [this, w, mute, term, next, ms] {
                    const auto took = ms(Clock::now() - *term);
                    note(fmt::format("owed: {} exited {} ms after SIGTERM", w.name, took));
                    // The mute one waits for the probe (700 ms from the
                    // request, 650 from SIGTERM), not for the budget (1500).
                    if (w.pid == mute.pid
                        && !expect(took >= 550 && took < 1300,
                                   fmt::format("owed: {} exited {} ms after SIGTERM — the stop did "
                                               "not wait for its asker's 700 ms, or waited for "
                                               "the budget instead", w.name, took)))
                        return;
                    next();
                });
        });
    }

    void step_owed_respawned(IpcPeer late, IpcPeer mute, std::function<void()> then)
    {
        wait_until([this, late, mute] {
            if (!all_live())
                return false;
            for (const auto& p : ipc().peers(ipc_workers))
                if (p.pid == late.pid || p.pid == mute.pid)
                    return false;
            return true;
        }, Clock::now() + 10s, "the stopped workers to be respawned", [this, then] {
            remember_workers();
            note("owed: both respawned");
            then();
        });
    }

    void step_owed_asker()
    {
        const IpcPeer asker  = workers_[1];
        const IpcPeer holder = workers_[2];
        ipc().request(asker.slot, "ask", fmt::format("{} 5000", holder.slot),
                      [this, asker, holder](IpcResult r) {
            if (!expect(r.ok() && r.body == "asked",
                        fmt::format("owed: {} did not ask: {} '{}'", asker.name, ipc_error_name(r.error), r.body)))
                return;
            // The asker owes the probe an answer 1.5 s from now: it stays on
            // the node, stopping, while the holder stops — so it is its
            // stopping, not its exit, that releases the holder.
            ipc().request(asker.slot, "later", "1500", [](IpcResult) {}, 3s);
            // The holder has the request by now; the asker stops first.
            after(100ms, [this, asker, holder] {
                ::kill(asker.pid, SIGTERM);
                after(200ms, [this, asker, holder] {
                    const auto term = Clock::now();
                    ::kill(holder.pid, SIGTERM);
                    wait_until([pid = holder.pid] { return !alive(pid); }, Clock::now() + 10s,
                               holder.name + " to exit after SIGTERM", [this, asker, holder, term] {
                        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - term).count();
                        note(fmt::format("owed: {} exited {} ms after SIGTERM, its asker {} stopped",
                                         holder.name, took, asker.name));
                        if (!expect(took < 700,
                                    fmt::format("owed: {} exited {} ms after SIGTERM — it waited for an "
                                                "asker that had stopped waiting", holder.name, took)))
                            return;
                        step_owed_respawned(asker, holder, [this] {
                            step_sockets(15, [this] { step_reload(); });
                        });
                    });
                });
            });
        }, 3s);
    }

    // ── 9. reload ────────────────────────────────────────────────────────────

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
                check_first(ipc().peers(ipc_workers | ipc_helper), 0, [this, old] {
                    note("new generation's first gathers answered by all");
                    wait_until([old] {
                        for (auto o : *old)
                            if (alive(o))
                                return false;
                        return true;
                    }, Clock::now() + 10s, "the old workers to exit", [this, old] {
                        for (auto o : *old)
                        {
                            const auto v = read_file(detach_file(o));
                            if (!expect(v == "stopping stopping stopping() stopping()\n",
                                        fmt::format("worker {} stopping with a request in flight: "
                                                    "'{}', not both callbacks 'stopping', each seeing "
                                                    "stopping()", o, v)))
                                return;
                        }
                        note("old workers: both callbacks ran as the stop began");
                        pass();
                    });
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
        module_manager().add_module(std::make_unique<SlowStop>(*this));
        install(*this, loop);
    }

    // Before the worker attaches its channels: the one the probe asked for
    // (step 7) sleeps, so that a request reaches its inbox first.
    void worker_run() override
    {
        std::error_code ec;
        if (fs::remove(slow_file(), ec))
        {
            std::this_thread::sleep_for(500ms);
            write_file(woke_file(), std::to_string(mono_ms()));
        }
        Application::worker_run();
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
