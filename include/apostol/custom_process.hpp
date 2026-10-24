#pragma once

#ifdef WITH_POSTGRESQL

#include <chrono>
#include <string_view>

namespace apostol
{

class EventLoop;
class Application;

// ─── CustomProcess ───────────────────────────────────────────────────────────
//
// Base class for custom background processes.
// Mirrors Module lifecycle (on_start / heartbeat / on_stop) but runs
// in its own forked process with dedicated EventLoop and PgPool.
//
// Subclass and override on_start() to set up timers, LISTEN channels,
// BotSession, etc. Register via Application::add_custom_process().
//
// Process lifecycle (managed by Application::custom_process_run()):
//   1. Signal unblock + crash handler + process title
//   2. EventLoop + signal handlers (SIGTERM, SIGQUIT, SIGUSR1)
//   3. PgPool setup (if pg_conninfo_helper configured)
//   4. on_start(loop, app)  — user setup
//   5. heartbeat timer (1s) — calls heartbeat() every second
//   6. loop.run()           — event loop
//   7. stop_work()          — heartbeat timers cancelled, on_stopping(),
//                             notifications muted, work in flight finished
//                             (busy() and the pool, within process.stop_timeout),
//                             pools detached, then on_stop() (user cleanup),
//                             then its queries drained
//   8. stop_db()            — destroy PgPool while EventLoop still alive
//
class CustomProcess
{
public:
    virtual ~CustomProcess() = default;

    /// Process name — used in config lookup and registration.
    virtual std::string_view name() const = 0;

    /// Display name shown in process title (defaults to name()).
    virtual std::string_view title() const { return name(); }

    /// Called once after EventLoop + PgPool are ready.
    /// Set up timers, LISTEN channels, BotSession, etc.
    virtual void on_start(EventLoop& loop, Application& app) = 0;

    /// Called every second (like Module::heartbeat).
    virtual void heartbeat(std::chrono::system_clock::time_point now)
    {
        (void)now;
    }

    /// The process starts to stop: take nothing new, finish what is started.
    /// The heartbeat is no longer called. Same contract as Module::on_stopping().
    virtual void on_stopping() {}

    /// True while work started before on_stopping() is not finished; the stop
    /// waits for it within "process.stop_timeout". Same as Module::busy().
    virtual bool busy() const { return false; }

    /// Called before EventLoop destruction, after the work in flight has been
    /// drained. Clean up resources. Same guarantees as Module::on_stop(): no
    /// handler of a query sent before this call runs after it; non-database
    /// callbacks are not cut off.
    virtual void on_stop() {}
};

} // namespace apostol

#endif // WITH_POSTGRESQL
