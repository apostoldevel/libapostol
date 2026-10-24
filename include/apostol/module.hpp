#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

class HttpRequest;
class HttpResponse;

// ─── Module ───────────────────────────────────────────────────────────────────

/// Base class for all Apostol application modules.
/// Each module declares whether it is active (enabled()) and optionally
/// handles individual HTTP requests (execute()) or periodic ticks (heartbeat()).
class Module
{
public:
    virtual ~Module() = default;

    /// Identifier used in config lookup and module registration.
    virtual std::string_view name() const = 0;

    /// Display name shown in process title and logs (defaults to name()).
    virtual std::string_view title() const { return name(); }

    /// When false the module is excluded from both execute and heartbeat.
    virtual bool enabled() const = 0;

    /// Handle an HTTP request.
    /// @return true  — request was handled; no further modules are tried.
    /// @return false — this module does not own the request; continue dispatch.
    virtual bool execute(const HttpRequest& req, HttpResponse& resp) = 0;

    /// Called periodically (typically every second) by the worker timer.
    virtual void heartbeat(std::chrono::system_clock::time_point /*now*/) {}

    /// Called once when the worker event loop starts.
    virtual void on_start() {}

    /// Called once when the process starts to stop, before the work in flight
    /// is waited for (Application::stop_work): take nothing new from here on —
    /// no new subscription, poll pass or session — and finish what is started.
    /// The heartbeat is no longer called. execute() still is, for a request on a
    /// connection already open: it is the last on that connection.
    virtual void on_stopping() {}

    /// True while work started before on_stopping() is not finished. The stop
    /// waits until no module is busy, nothing is owed on an HTTP connection and
    /// no query of a pool is outstanding — or until its time ("process.stop_timeout")
    /// runs out. Queries and HTTP answers are counted by the framework; this is
    /// for the rest: an outbound HTTP call, an SMTP send, a step on a timer.
    virtual bool busy() const { return false; }

    /// Called once after the event loop has stopped and the work in flight has
    /// been drained (Application::stop_work). From here on no handler of a query
    /// sent before this call is invoked; a query sent from on_stop() itself is
    /// delivered if it completes within the drain that follows. Callbacks that do
    /// not come from the database — timers, sockets, SMTP, HTTP clients — are not
    /// cut off by the framework: what they use must outlive them, or be closed here.
    virtual void on_stop() {}
};

// ─── ModuleManager ────────────────────────────────────────────────────────────

/// Owns a list of Module instances and dispatches HTTP requests and heartbeats.
class ModuleManager
{
public:
    /// Transfer ownership of a module to this manager.
    void add_module(std::unique_ptr<Module> m);

    /// Iterate enabled modules in registration order.
    /// Stops at the first module whose execute() returns true.
    /// @return true if a module handled the request, false otherwise.
    bool execute(const HttpRequest& req, HttpResponse& resp);

    /// Call heartbeat() on every enabled module.
    void heartbeat(std::chrono::system_clock::time_point now);

    /// Number of registered modules (enabled + disabled).
    std::size_t count() const noexcept { return modules_.size(); }

    /// Access a module by index (for inspection / tests).
    Module*       module(std::size_t i)       { return modules_.at(i).get(); }
    const Module* module(std::size_t i) const { return modules_.at(i).get(); }

    void on_start();

    /// The process starts to stop: heartbeat() does nothing from here on, and
    /// every enabled module is told by on_stopping(). execute() still serves —
    /// quiesce() ends that. A module that throws is reported to @p on_error and
    /// the others are still told.
    void stopping(const std::function<void(const Module&, const std::exception&)>& on_error = {});

    /// True while an enabled module reports busy().
    bool busy() const;

    /// Comma-separated names of the enabled modules that report busy() (for the log).
    std::string busy_names() const;

    /// Latch without stopping anything: execute() refuses and heartbeat() does
    /// nothing from here on, while the modules stay intact. A shutdown calls it
    /// once the work in flight is finished (Application::stop_work) and before
    /// on_stop(), so that what is left of the loop starts nothing new.
    void quiesce() noexcept { stopped_ = true; }

    /// Stop every enabled module, and latch: after this, execute() refuses and
    /// heartbeat() does nothing.
    ///
    /// The latch matters because the event loop may still run afterwards — a
    /// shutdown drains pending database work (Application::drain_db), and during
    /// that window the one-second heartbeat timer keeps firing and a connection
    /// still open may bring a request. Without it a module that released its session in
    /// on_stop() would be asked to work again: BotSession, finding itself invalid,
    /// logs in anew and leaves behind exactly the session the drain exists to close.
    void on_stop();

    /// True once the stop has begun — stopping(), quiesce() or on_stop() has
    /// run. Modules read it as "the process is stopping, start nothing new"
    /// (an answer arriving now must not send a request out), and it has to say
    /// so from the first moment: execute() goes on serving until quiesce(),
    /// which comes only after the work in flight is finished.
    bool stopped() const noexcept { return stopping_ || stopped_; }

    /// Comma-separated names of enabled modules (for process title).
    std::string module_names() const;

private:
    std::vector<std::unique_ptr<Module>> modules_;
    bool stopping_{false};
    bool stopped_{false};
};

} // namespace apostol
