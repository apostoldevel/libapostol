#pragma once

#include <chrono>
#include <cstddef>
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

    /// Called once when the worker event loop is about to stop.
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

    /// Stop every enabled module, and latch: after this, execute() refuses and
    /// heartbeat() does nothing.
    ///
    /// The latch matters because the event loop may still run afterwards — a
    /// shutdown drains pending database work (Application::drain_db), and during
    /// that window the one-second heartbeat timer keeps firing and the listening
    /// socket keeps accepting. Without it a module that released its session in
    /// on_stop() would be asked to work again: BotSession, finding itself invalid,
    /// logs in anew and leaves behind exactly the session the drain exists to close.
    void on_stop();

    /// True once on_stop() has run.
    bool stopped() const noexcept { return stopped_; }

    /// Comma-separated names of enabled modules (for process title).
    std::string module_names() const;

private:
    std::vector<std::unique_ptr<Module>> modules_;
    bool stopped_{false};
};

} // namespace apostol
