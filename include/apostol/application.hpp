#pragma once

#include "apostol/config.hpp"
#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/module.hpp"
#include "apostol/oauth_providers.hpp"
#include "apostol/site_config.hpp"
#ifdef WITH_POSTGRESQL
#include "apostol/custom_process.hpp"
#include "apostol/pg.hpp"
#include "apostol/process_module.hpp"
#endif
#include "apostol/process.hpp"
#include "apostol/settings.hpp"
#include "apostol/websocket.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

class TcpListener;  // forward declaration for master_listener_

// ─── Application ─────────────────────────────────────────────────────────────
//
// Base class for Apostol applications. Manages the master/worker process model.
//
// Lifecycle (master process):
//   1. parse_args()            — read CLI flags
//   2. init_logging()          — set up logger
//   3. load_config()           — read JSON config
//   4. check_running() / daemonize() / write_pid()
//   5. on_start()              — virtual hook, override in subclass
//   6. spawn_workers()         — fork N workers
//   7. master_run()            — enter signal-driven event loop
//   8. remove_pid()            — cleanup on exit
//
// Worker process (after fork):
//   1. Reset signal mask
//   2. on_worker_start(loop)   — virtual hook, add HTTP server etc.
//   3. loop.run()
//
// Signals handled by master:
//   SIGTERM / SIGINT  → fast shutdown (SIGTERM to all children → exit)
//   SIGQUIT           → graceful shutdown (SIGQUIT to children, wait for exit)
//   SIGHUP            → reload config + rolling restart of workers
//   SIGWINCH          → gracefully stop workers (keep master running)
//   SIGCHLD           → reap exited children, respawn if not shutting down
//
class Application
{
public:
    explicit Application(std::string_view name = "apostol");
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Main entry point — call from main().
    int run(int argc, char* argv[]);

    std::string_view app_name() const noexcept { return name_; }
    ProcessRole role() const noexcept { return role_; }

    Logger&        logger()         noexcept { return *logger_; }
    const Config&  config()   const noexcept { return *config_; }
    ModuleManager& module_manager() noexcept { return module_manager_; }

    /// Set up a non-blocking HTTP server on @p port (0 = OS picks).
    /// Registers accept + read handlers in @p loop, and a 1-second heartbeat
    /// timer for module_manager().heartbeat(). Must be called from
    /// on_worker_start() or directly from test code.
    void start_http_server(EventLoop& loop, uint16_t port = 0);

    /// Returns the port the HTTP server is listening on (valid after
    /// start_http_server() has been called; 0 if not started).
    uint16_t http_port() const noexcept { return http_port_; }

    // ── Application identity (call before run()) ──────────────────────────────
    void set_info(std::string_view name,
                  std::string_view version     = {},
                  std::string_view description = {});

    /// Access validated settings (available after load_config())
    const AppSettings& settings() const noexcept { return settings_; }

#ifdef WITH_POSTGRESQL
    // ── PostgreSQL pool ───────────────────────────────────────────────────────

    /// Create and start a PgPool with @p conninfo, @p min_conns .. @p max_conns.
    /// Returns a reference to the pool (owned by this Application).
    /// Must be called from on_worker_start() before start_http_server().
    PgPool& setup_db(EventLoop& loop, std::string conninfo,
                     std::size_t min_conns = 1, std::size_t max_conns = 5);

    /// Access the PgPool created by setup_db(). Throws if setup_db() was not
    /// called first.
    PgPool& db_pool();

    /// True if setup_db() was called and the pool is alive.
    bool has_db_pool() const noexcept { return db_pool_ != nullptr; }

    /// Explicitly destroy the PgPool while @p loop is still alive.
    /// Call this at the end of on_worker_start() (or test helper) BEFORE
    /// the EventLoop is destroyed; PgPool::~PgPool() calls loop_.remove_io()
    /// which requires the loop to be alive.
    void stop_db() noexcept { db_pool_.reset(); }
#else
    void stop_db() noexcept {} // no-op when built without PostgreSQL
#endif // WITH_POSTGRESQL

    // ── WebSocket handler ─────────────────────────────────────────────────────

    /// Callback type invoked when a WebSocket upgrade is accepted.
    /// The handler owns the WsConnection and must register it with @p loop.
    using WsHandler =
        std::function<void(EventLoop&, WsConnection, const HttpRequest&)>;

    /// Register a handler called for each incoming WebSocket upgrade request.
    /// If no handler is registered, upgrade requests receive 404.
    void set_ws_handler(WsHandler h);

    /// Read "module.<name>.enable" from config (default: true).
    /// Mirrors v1 CApostolModule::Enabled() / [module/<Name>] enable=true pattern.
    /// Public so create_workers() / create_helpers() / create_processes() in
    /// Workers.hpp / Helpers.hpp / Processes.hpp can check module enabled state.
    bool module_enabled(std::string_view name, bool default_val = true) const;

    /// Return pointer to "module.<name>" JSON section, or nullptr if absent.
    /// Never throws. Convenience for create_workers()/create_helpers().
    const nlohmann::json* module_config(std::string_view name) const noexcept;

    /// Resolve @p path against settings().prefix. Absolute paths returned as-is.
    /// Empty @p path uses @p default_name. Convenience for file path config.
    std::filesystem::path resolve_path(std::string_view path,
                                       std::string_view default_name = "files") const;

    /// EventLoop of the current worker/single process (valid after on_worker_start).
    /// Provides EventLoop access to modules without changing create_workers() signature.
    EventLoop& worker_loop() { return *worker_loop_; }

    /// Set the worker event loop (for tests / custom setup).
    void set_worker_loop(EventLoop& loop) { worker_loop_ = &loop; }

    /// Access the centralized OAuth2 provider cache (loaded at startup).
    const OAuthProviders& providers() const noexcept { return providers_; }

    /// Load OAuth2 providers from a directory (for tests / custom setup).
    void load_providers(const std::filesystem::path& dir) { providers_.load(dir); }

    /// Access site configurations (loaded from conf/sites/*.json).
    const SiteConfigs& sites() const noexcept { return sites_; }

    /// Dedicated stream logger (writes to stream.log). Created on demand.
    Logger& stream_logger();

protected:
    // ── Virtual hooks ─────────────────────────────────────────────────────────

    // Called in master/single once, before workers are spawned (or loop starts).
    virtual void on_start() {}

    // Called in master after config is reloaded (SIGHUP).
    virtual void on_reload() {}

    // Called in each worker (and single) process. Register HTTP handlers here.
    virtual void on_worker_start(EventLoop& /*loop*/) {}

    // Called in the helper process.
    virtual void on_helper_start(EventLoop& /*loop*/) {}

    // Override to register custom background processes via add_custom_process().
    virtual void create_custom_processes() {}

private:
    // ── Startup ───────────────────────────────────────────────────────────────
    void parse_args(int argc, char* argv[]);
    void init_logging();
    void load_config();
    void write_pid_file() const;
    void remove_pid_file() const;
    bool check_running() const;
    static void daemonize();
    void send_signal_to_running(std::string_view sig_name) const;

    void print_version() const;
    void print_version_info() const;
    void create_directories();

// NOTE: start_process / run-loops / spawn_* are in the protected section below

    void fast_shutdown();
    void graceful_shutdown();
    void rolling_restart();

    // ── OS-level helpers ──────────────────────────────────────────────────────
    void init_setproctitle(int argc, char* argv[]);
    void set_process_title(std::string_view title);
    static void set_limit_nofile(std::uint32_t limit);
    static void set_user(std::string_view user, std::string_view group);

    // ── Config shortcuts (read after load_config()) ───────────────────────────
    bool          cfg_master()       const { return settings_.master; }
    bool          cfg_helper()       const { return settings_.helper; }
    std::uint32_t cfg_limit_nofile() const { return settings_.limit_nofile; }
    std::string   cfg_user()         const { return settings_.user; }
    std::string   cfg_group()        const { return settings_.group; }
    std::string   cfg_prefix()       const { return settings_.prefix; }

    // ── Custom process descriptor ─────────────────────────────────────────────
#ifdef WITH_POSTGRESQL
    struct CustomProcessEntry
    {
        std::string name;
        std::unique_ptr<CustomProcess> process;
    };
#else
    struct CustomProcessEntry
    {
        std::string name;
        std::function<void(EventLoop&)> fn;
    };
#endif

public:
    /// Register a custom background process to be forked by the master.
    /// Must be called from create_custom_processes() override.
    /// Requires WITH_POSTGRESQL — CustomProcess uses PgPool + BotSession.
#ifdef WITH_POSTGRESQL
    void add_custom_process(std::unique_ptr<CustomProcess> proc);

    /// Convenience overload: wraps a ProcessModule in a generic ModuleProcess.
    /// Use this for processes that only need PG + heartbeat (no custom infra).
    void add_custom_process(std::unique_ptr<ProcessModule> mod);
#else
    void add_custom_process(std::string name, std::function<void(EventLoop&)> fn);
#endif

protected:
    // ── Mutable state (accessible to test subclasses) ─────────────────────────
    ProcessRole role_{ProcessRole::single}; // default: single (no fork)
    AppSettings settings_;                  // populated by load_config(); writable for test subclasses

    // ── Process dispatch + run loops (protected virtual for testability) ───────
    void start_process();

    virtual void master_run();
    virtual void single_run();
    virtual void worker_run();
    virtual void helper_run();
#ifdef WITH_POSTGRESQL
    void custom_process_run(CustomProcess& proc);
#endif

    pid_t fork_child(ProcessRole role, std::string name,
                     std::function<void()> custom_fn = {});
    virtual void spawn_workers();
    virtual void spawn_helper();
    void reap_children();

private:
    // ── State ─────────────────────────────────────────────────────────────────
    std::string name_;

    // Parsed options
    std::filesystem::path config_file_; // set from settings_.conf_file, then optionally overridden by -c
    bool daemon_{false};
    bool test_config_{false};
    bool show_version_{false};
    bool show_configure_{false};
    std::string send_signal_; // non-empty if -s flag was given
    std::string locale_;
    std::string conf_param_;  // global config directives (-g)
    std::string cmdline_;     // original command line (for process title)
    int         cli_workers_{-1}; // -1 = not set via CLI; ≥0 = explicit -w value
    int         os_argc_{0};
    char**      os_argv_{nullptr};
    char*       os_argv_last_{nullptr};  // end of contiguous argv+environ memory
    char*       os_environ_{nullptr};    // heap copy of environ
    int         exit_code_{0};

    std::unique_ptr<Logger>  logger_;
    std::unique_ptr<Config>  config_;
    ModuleManager            module_manager_;
    OAuthProviders           providers_;
    SiteConfigs              sites_;
    uint16_t                 http_port_{0};

#ifdef WITH_POSTGRESQL
    std::unique_ptr<PgPool>   db_pool_;
    std::unique_ptr<Logger>   pg_logger_;
#endif
    WsHandler                ws_handler_;
    EventLoop*               worker_loop_{nullptr};
    std::unique_ptr<Logger>  stream_logger_;

    std::unique_ptr<TcpListener> master_listener_;  // listening socket (master only)
    int listen_fd_{-1};                              // fd inherited by workers via fork

    std::vector<ChildInfo>            children_;
    std::vector<CustomProcessEntry>   custom_processes_;
    bool shutting_down_{false};
    bool graceful_{false}; // true → wait for workers before exiting

    // Seconds to wait for workers to exit after SIGTERM before sending SIGKILL.
    // Mirrors v1 ~1.55 s deadline (50→100→200→400→800 ms doubling).
    // Can be changed by subclass before run() if needed.
    int kill_timeout_secs_{5};
};

} // namespace apostol
