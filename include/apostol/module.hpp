#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
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

    /// Identifier shown in logs.
    virtual std::string_view name() const = 0;

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
    void on_stop();

private:
    std::vector<std::unique_ptr<Module>> modules_;
};

} // namespace apostol
