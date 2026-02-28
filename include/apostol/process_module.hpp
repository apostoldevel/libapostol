#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/custom_process.hpp"

#include <memory>
#include <string_view>

namespace apostol
{

// ─── ProcessModule ───────────────────────────────────────────────────────────
//
// Abstract base class for background process business logic.
//
// Mirrors the v1 pattern where a process (CBSProxy, CTaskScheduler) owns a
// module (CBSModule) that contains all the actual logic.  In v2 the process
// shell is generic (ModuleProcess) and only the module needs to be written.
//
// Usage:
//   class MyLogic final : public ProcessModule {
//       std::string_view name() const override { return "my-logic"; }
//       void on_start(EventLoop& loop, Application& app) override { ... }
//       void heartbeat(time_point now) override { ... }
//       void on_stop() override { ... }
//   };
//
//   // In Processes.hpp:
//   app.add_custom_process(std::make_unique<MyLogic>());
//
class ProcessModule
{
public:
    virtual ~ProcessModule() = default;

    /// Module name — used in logs and process title.
    virtual std::string_view name() const = 0;

    /// Called once after EventLoop + PgPool are ready.
    virtual void on_start(EventLoop& loop, Application& app) = 0;

    /// Called every second.
    virtual void heartbeat(std::chrono::system_clock::time_point now)
    {
        (void)now;
    }

    /// Called before EventLoop destruction.
    virtual void on_stop() {}
};

// ─── ModuleProcess ───────────────────────────────────────────────────────────
//
// Generic CustomProcess that wraps a ProcessModule.
//
// This is the "process template" — it handles the full POSIX lifecycle
// (signal unblock, crash handler, EventLoop, PgPool, heartbeat timer)
// and delegates all business logic to the injected ProcessModule.
//
// For processes that need custom infrastructure (e.g. HTTP server,
// WebSocket client), subclass CustomProcess directly instead.
//
class ModuleProcess final : public CustomProcess
{
public:
    explicit ModuleProcess(std::unique_ptr<ProcessModule> mod)
        : module_(std::move(mod))
    {}

    std::string_view name() const override { return module_->name(); }

    void on_start(EventLoop& loop, Application& app) override
    {
        module_->on_start(loop, app);
    }

    void heartbeat(std::chrono::system_clock::time_point now) override
    {
        module_->heartbeat(now);
    }

    void on_stop() override
    {
        module_->on_stop();
    }

private:
    std::unique_ptr<ProcessModule> module_;
};

} // namespace apostol

#endif // WITH_POSTGRESQL
