#include "apostol/application.hpp"
#include "apostol/crash_handler.hpp"
#ifdef WITH_POSTGRESQL
#include "apostol/custom_process.hpp"
#include "apostol/process_module.hpp"
#endif
#include "apostol/http.hpp"
#include "apostol/settings.hpp"
#include "apostol/tcp.hpp"
#ifdef WITH_POSTGRESQL
#include "apostol/pg.hpp"
#endif
#include "apostol/websocket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <locale.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apostol
{

// ─── Construction ────────────────────────────────────────────────────────────

Application::Application(std::string_view name) : name_(name), logger_(std::make_unique<Logger>())
{
    // Sync name_ into settings so set_info() and app_name() are consistent
    settings_.name = name_;
    // Default config_file_ from settings (CMake defaults already resolved in AppSettings ctor)
    config_file_ = settings_.conf_file;

    // Default: log to stderr at info level until config is loaded
    logger_->add_target(std::make_unique<StderrTarget>());
    logger_->set_level(LogLevel::info);
}

Application::~Application()
{
    delete[] os_environ_;
}

// ─── set_info ────────────────────────────────────────────────────────────────

void Application::set_info(std::string_view name,
                            std::string_view version,
                            std::string_view description)
{
    if (!name.empty())        { settings_.name    = name;    name_ = settings_.name; }
    if (!version.empty())     settings_.version     = version;
    if (!description.empty()) settings_.description = description;
}

bool Application::module_enabled(std::string_view name, bool default_val) const
{
    return config_->get_bool(fmt::format("module.{}.enable", name), default_val);
}

const nlohmann::json* Application::module_config(std::string_view name) const noexcept
{
    try {
        if (!config_)
            return nullptr;
        auto& cfg = config_->json();
        if (!cfg.contains("module"))
            return nullptr;
        auto& modules = cfg["module"];
        auto it = modules.find(name);
        if (it == modules.end())
            return nullptr;
        return &(*it);
    } catch (...) {
        return nullptr;
    }
}

std::filesystem::path Application::resolve_path(std::string_view path,
                                                 std::string_view default_name) const
{
    if (path.empty())
        return std::filesystem::path(settings_.prefix) / std::string(default_name);
    if (!path.empty() && path[0] == '/')
        return std::filesystem::path(path);
    return std::filesystem::path(settings_.prefix) / std::string(path);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int Application::run(int argc, char* argv[])
{
    init_setproctitle(argc, argv);

    // Build cmdline string for use in process titles
    cmdline_.clear();
    for (int i = 0; i < argc; ++i)
    {
        if (i > 0) cmdline_ += ' ';
        cmdline_ += argv[i];
    }

    // Default config file comes from AppSettings CMake defaults
    // (will be overridden by -c flag in parse_args if provided)
    if (config_file_.empty())
        config_file_ = settings_.conf_file;

    parse_args(argc, argv);

    // Resolve config file against prefix if a relative path was given via -c
    // (handles both orderings: -p before -c and -c before -p)
    if (config_file_.is_relative() && !settings_.prefix.empty())
        config_file_ = settings_.prefix / config_file_;

    if (show_version_)
    {
        print_version();
        return 0;
    }
    if (show_configure_)
    {
        print_version_info();
        return 0;
    }

    // -s: send signal to running instance and exit
    if (!send_signal_.empty())
    {
        role_ = ProcessRole::signaller;
        send_signal_to_running(send_signal_);
        return 0;
    }

    init_logging();
    load_config();

    // Install crash handler now that we know the error log path.
    // Re-registers in every child process too (see worker_run/helper_run/single_run).
    install_crash_handler(settings_.error_log.string());

    // CLI -w overrides config workers (mirrors v1 DefaultCommands: skip config if CLI set)
    if (cli_workers_ >= 0)
        settings_.workers = cli_workers_;

    // CLI -l overrides config locale; then apply to process (mirrors v1 DefaultLocale.SetLocale)
    if (!locale_.empty())
        settings_.locale = locale_;
    if (!settings_.locale.empty())
        if (::setlocale(LC_ALL, settings_.locale.c_str()) == nullptr)
            logger_->warn("setlocale('{}') failed — using system default locale",
                settings_.locale);

    if (test_config_)
    {
        logger_->notice("configuration '{}' test is successful", config_file_.string());
        return 0;
    }

    create_directories();

    // CLI -d takes priority; fall back to daemon.enabled from config
    if (!daemon_)
        daemon_ = settings_.daemon;

    if (daemon_)
        daemonize();

    try
    {
        start_process();
    }
    catch (const std::exception& e)
    {
        if (logger_)
            logger_->error("fatal: {}", e.what());
        else
            std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return exit_code_;
}

// ─── Version / configure info ─────────────────────────────────────────────────

void Application::print_version() const
{
    std::fprintf(stdout, "%s version: %s\n", name_.c_str(),
#ifdef APP_VERSION
        APP_VERSION
#else
        "unknown"
#endif
    );
}

void Application::print_version_info() const
{
    print_version();
    std::fprintf(stdout, "configure options:\n");
#ifdef APP_CONFIGURE_STRING
    std::fprintf(stdout, "  %s\n", APP_CONFIGURE_STRING);
#else
    std::fprintf(stdout, "  (no configure info)\n"
        "  WITH_POSTGRESQL="
#ifdef WITH_POSTGRESQL
        "ON"
#else
        "OFF"
#endif
        "\n");
#endif
}

// ─── Process dispatch ─────────────────────────────────────────────────────────

void Application::start_process()
{
    // Helper flag in config → switch role to helper
    if (cfg_helper())
        role_ = ProcessRole::helper;

    if (role_ != ProcessRole::signaller)
    {
        create_custom_processes(); // virtual hook — override to register custom processes

        // master mode only if explicitly configured
        if (cfg_master())
            role_ = ProcessRole::master;
    }

    if (check_running())
    {
        logger_->error("{} is already running (pid file: {})", name_, settings_.pid_file.string());
        exit_code_ = 1;
        return;
    }

    if (role_ == ProcessRole::master)
    {
        write_pid_file();
        on_start();

        // Create listening socket BEFORE fork (nginx model).
        // Workers inherit fd and only call accept().
        if (settings_.server_port != 0) {
            master_listener_ = std::make_unique<TcpListener>(
                settings_.server_port, settings_.server_backlog);
            listen_fd_ = master_listener_->fd();
            http_port_ = master_listener_->local_port();
            logger_->notice("master: bound listening socket on port {}", http_port_);
        }

        if (cfg_helper()) spawn_helper();
        // Spawn custom processes first, then workers
        for (auto& cp : custom_processes_)
        {
#ifdef WITH_POSTGRESQL
            fork_child(ProcessRole::custom, cp.name, [this, &cp] {
                custom_process_run(*cp.process);
            });
#else
            auto fn = cp.fn; // copy for lambda capture
            fork_child(ProcessRole::custom, cp.name, [this, fn] {
                EventLoop loop;
                loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
                loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
                fn(loop);
                loop.run();
            });
#endif
        }
        spawn_workers();
        master_run();

        master_listener_.reset();
        listen_fd_ = -1;

        remove_pid_file();
    }
    else if (role_ == ProcessRole::helper)
    {
        write_pid_file();
        on_start();
        helper_run();
        remove_pid_file();
    }
    else
    {
        // Default: single process — no fork
        role_ = ProcessRole::single;
        write_pid_file();
        on_start();
        single_run();
        remove_pid_file();
    }
}

// ─── CLI parsing ─────────────────────────────────────────────────────────────

void Application::parse_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];

        if (arg == "-d" || arg == "--daemon")
        {
            daemon_ = true;
        }
        else if (arg == "-t" || arg == "--test")
        {
            test_config_ = true;
        }
        else if ((arg == "-c" || arg == "--config") && i + 1 < argc)
        {
            config_file_ = argv[++i];
        }
        else if ((arg == "-p" || arg == "--prefix") && i + 1 < argc)
        {
            settings_.prefix = argv[++i];
            if (!settings_.prefix.empty() && settings_.prefix.back() != '/')
                settings_.prefix += '/';
            // Re-resolve all default paths with new prefix
            settings_.conf_file    = settings_.resolve(std::string(APP_PREFIX) + APP_CONF_FILE);
            settings_.pid_file     = settings_.resolve(APP_PID_FILE);
            settings_.lock_file    = settings_.resolve(APP_LOCK_FILE);
            settings_.error_log    = settings_.resolve(APP_ERROR_LOG_FILE);
            settings_.access_log   = settings_.resolve(APP_ACCESS_LOG_FILE);
            settings_.stream_log   = settings_.resolve(APP_STREAM_LOG_FILE);
            settings_.postgres_log = settings_.resolve(APP_POSTGRES_LOG_FILE);
            settings_.doc_root     = settings_.resolve(APP_DOC_ROOT);
            settings_.cache_prefix = settings_.resolve(APP_CACHE_PREFIX);
            // Sync config_file_ if not yet overridden by -c
            if (config_file_.empty() || config_file_ == settings_.conf_file)
                config_file_ = settings_.conf_file;
        }
        else if ((arg == "-s" || arg == "--signal") && i + 1 < argc)
        {
            send_signal_ = argv[++i];
        }
        else if ((arg == "-w" || arg == "--workers") && i + 1 < argc)
        {
            cli_workers_ = std::atoi(argv[++i]);
            if (cli_workers_ < 0)
                cli_workers_ = 0; // 0 = auto (effective_workers() → nproc)
        }
        else if (arg == "-v" || arg == "--version")
        {
            show_version_ = true;
        }
        else if (arg == "-V")
        {
            show_configure_ = true;
        }
        else if ((arg == "-l" || arg == "--locale") && i + 1 < argc)
        {
            locale_ = argv[++i];
        }
        else if ((arg == "-g" || arg == "--global") && i + 1 < argc)
        {
            conf_param_ = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::fprintf(stdout,
                "Usage: %s [options]\n"
                "  -c, --config <file>    configuration file (default: conf/apostol.json)\n"
                "  -p, --prefix <path>    set installation prefix path\n"
                "  -s, --signal <signal>  send signal: stop, quit, reload, reopen\n"
                "  -w, --workers <n>      number of worker processes\n"
                "  -d, --daemon           run as daemon\n"
                "  -t, --test             test configuration and exit\n"
                "  -v, --version          print version\n"
                "  -V                     print version and configure options\n"
                "  -l, --locale <locale>  set locale\n"
                "  -g, --global <directives>  set global config directives\n"
                "  -h, --help             show this help\n",
                name_.c_str());
            std::exit(0);
        }
    }
}

// ─── Initialization ──────────────────────────────────────────────────────────

void Application::init_logging()
{
    // Config not yet loaded — keep stderr target; update level once config is read.
    // Actual file logging is set up after load_config().
}

void Application::load_config()
{
    if (!std::filesystem::exists(config_file_))
    {
        logger_->notice("config file '{}' not found, using defaults",
            config_file_.string());
        config_ = std::make_unique<Config>(Config::from_string("{}"));
    }
    else
    {
        try
        {
            config_ = std::make_unique<Config>(Config::from_file(config_file_));
        }
        catch (const ConfigError& e)
        {
            logger_->error("failed to load config: {}", e.what());
            throw;
        }
    }

    // Populate AppSettings from JSON (CMake defaults are the baseline)
    settings_.populate(*config_);

    // Strict validation — collect ALL errors before deciding what to do
    auto errors = settings_.validate();
    if (!errors.empty())
    {
        for (auto& e : errors)
            logger_->error("configuration error [{}]: {}", e.key, e.message);

        if (test_config_)
        {
            logger_->error("configuration test failed ({} error(s))", errors.size());
            throw ConfigError(fmt::format("configuration has {} error(s)", errors.size()));
        }
        else
        {
            logger_->warn("configuration has {} error(s) — applying defaults for invalid values",
                errors.size());
        }
    }

    // Apply log level from settings
    try
    {
        logger_->set_level(level_from_string(settings_.log_level));
    }
    catch (const std::invalid_argument&)
    {
        logger_->warn("unknown log level '{}', keeping current level", settings_.log_level);
    }

    // Open (or replace) log file if configured.
    // set_file_target removes any existing FileTargets before adding the new one,
    // so repeated load_config() calls on SIGHUP don't accumulate targets.
    if (!settings_.error_log.empty())
    {
        try
        {
            std::filesystem::create_directories(settings_.error_log.parent_path());
            logger_->set_file_target(settings_.error_log.string(), settings_.log_max_size,
                                     settings_.log_keep_rotated, settings_.log_compress);
        }
        catch (const std::exception& e)
        {
            logger_->warn("cannot open log file '{}': {}", settings_.error_log.string(), e.what());
        }
    }

    // Load OAuth2 provider configs (conf/oauth2/*.json).
    // clear() + load() is safe for repeated calls on SIGHUP.
    providers_.clear();
    providers_.load(settings_.resolve("conf/oauth2"));

    // Load site configs (conf/sites/*.json).
    sites_.clear();
    sites_.load(settings_.resolve("conf/sites"));
}

// ─── PID file ────────────────────────────────────────────────────────────────

void Application::write_pid_file() const
{
    std::filesystem::create_directories(settings_.pid_file.parent_path());
    std::ofstream f(settings_.pid_file);
    if (!f)
        throw std::system_error(errno, std::system_category(),
            fmt::format("cannot write PID file '{}'", settings_.pid_file.string()));
    f << ::getpid() << '\n';
}

void Application::remove_pid_file() const
{
    std::error_code ec;
    std::filesystem::remove(settings_.pid_file, ec);
}

bool Application::check_running() const
{
    if (!std::filesystem::exists(settings_.pid_file))
        return false;

    std::ifstream f(settings_.pid_file);
    pid_t pid = 0;
    f >> pid;
    if (pid <= 0)
        return false;

    // Check if the process is alive via kill(pid, 0):
    //   0     → process exists and we can signal it → running
    //   ESRCH → no such process → stale PID file
    //   EPERM → process exists but we lack permission → still running
    if (::kill(pid, 0) == 0 || errno == EPERM)
        return true;

    // errno == ESRCH: process is gone — stale PID file, remove and allow start
    logger_->notice("removing stale PID file '{}' (pid={} no longer exists)",
        settings_.pid_file.string(), pid);
    std::error_code ec;
    std::filesystem::remove(settings_.pid_file, ec);
    return false;
}

// ─── Daemonization ───────────────────────────────────────────────────────────

void Application::daemonize()
{
    // First fork — detach from terminal
    pid_t pid = ::fork();
    if (pid < 0)
        throw std::system_error(errno, std::system_category(), "fork (daemonize)");
    if (pid > 0)
        std::exit(0); // parent exits

    ::setsid();
    ::umask(0);

    // Second fork — ensure we can't acquire a controlling terminal
    pid = ::fork();
    if (pid < 0)
        throw std::system_error(errno, std::system_category(), "fork2 (daemonize)");
    if (pid > 0)
        std::exit(0);

    // Redirect stdin/stdout/stderr to /dev/null
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0)
    {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            ::close(devnull);
    }
}

// ─── Signal to running instance ──────────────────────────────────────────────

void Application::send_signal_to_running(std::string_view sig_name) const
{
    if (!std::filesystem::exists(settings_.pid_file))
    {
        std::fprintf(stderr, "apostol: PID file '%s' not found — is it running?\n",
            settings_.pid_file.c_str());
        return;
    }

    std::ifstream f(settings_.pid_file);
    pid_t pid = 0;
    f >> pid;
    if (pid <= 0)
    {
        std::fprintf(stderr, "apostol: invalid PID in '%s'\n", settings_.pid_file.c_str());
        return;
    }

    int signum = 0;
    if (sig_name == "stop")
        signum = SIGTERM;
    else if (sig_name == "quit")
        signum = SIGQUIT;
    else if (sig_name == "reload")
        signum = SIGHUP;
    else if (sig_name == "reopen")
        signum = SIGUSR1;
    else
    {
        std::fprintf(stderr, "apostol: unknown signal '%s' (use: stop, quit, reload, reopen)\n",
            std::string(sig_name).c_str());
        return;
    }

    if (::kill(pid, signum) < 0)
        std::fprintf(stderr, "apostol: kill(%d, %d): %s\n", pid, signum, std::strerror(errno));
    else
        std::fprintf(stdout, "apostol: signal '%s' sent to pid %d\n",
            std::string(sig_name).c_str(), pid);
}

// ─── Master loop ─────────────────────────────────────────────────────────────

void Application::master_run()
{
    set_process_title(fmt::format("{}: master process {}", name_, cmdline_));
    logger_->notice("{} master process started (pid={})", name_, ::getpid());

    EventLoop loop;

    // Timer ID for the SIGKILL escalation one-shot timer (armed on SIGTERM).
    EventLoop::TimerId kill_timer_id = EventLoop::kInvalidTimer;

    // SIGCHLD — child exited: reap, then stop loop once all children are gone.
    // Also cancels the kill escalation timer if it is still armed.
    loop.add_signal(SIGCHLD, [this, &loop, &kill_timer_id](const signalfd_siginfo&) {
        reap_children();
        if ((shutting_down_ || graceful_) && children_.empty())
        {
            if (kill_timer_id != EventLoop::kInvalidTimer)
            {
                loop.cancel_timer(kill_timer_id);
                kill_timer_id = EventLoop::kInvalidTimer;
            }
            loop.stop();
        }
    });

    // SIGTERM / SIGINT — fast shutdown:
    //   1. Send SIGTERM to all children and mark shutting_down_.
    //   2. Arm a one-shot SIGKILL escalation timer (mirrors v1 delay → SIGKILL path).
    //   3. SIGCHLD handler will cancel the timer and stop the loop if children
    //      exit before the deadline; otherwise the timer sends SIGKILL.
    auto fast_stop = [this, &loop, &kill_timer_id](const signalfd_siginfo&) {
        fast_shutdown();
        if (children_.empty())
        {
            loop.stop();
            return;
        }
        // Arm kill escalation timer (5 s default — mirrors v1 ~1.55 s deadline but
        // more generous to allow workers to finish in-flight requests).
        kill_timer_id = loop.add_timer(
            std::chrono::seconds(kill_timeout_secs_),
            [this, &loop, &kill_timer_id]() {
                kill_timer_id = EventLoop::kInvalidTimer;
                if (children_.empty())
                    return;
                logger_->warn("shutdown timeout ({} s) — sending SIGKILL to {} child(ren)",
                              kill_timeout_secs_, children_.size());
                for (auto& child : children_)
                    ::kill(child.pid, SIGKILL);
                // Proactively reap: zombies won't generate new SIGCHLD.
                reap_children();
                if (children_.empty())
                {
                    loop.stop();
                    return;
                }
                // If still not empty, SIGCHLD handler will catch the rest.
            },
            /*repeat=*/false);
    };
    loop.add_signal(SIGTERM, fast_stop);
    loop.add_signal(SIGINT, fast_stop);

    // SIGQUIT — graceful shutdown: tell workers to finish, wait for them
    loop.add_signal(SIGQUIT, [this, &loop](const signalfd_siginfo&) {
        graceful_shutdown();
        if (children_.empty())
            loop.stop(); // all already gone
    });

    // SIGHUP — reload config + rolling restart
    loop.add_signal(SIGHUP, [this](const signalfd_siginfo&) {
        logger_->notice("SIGHUP received — reloading config");
        rolling_restart();
        on_reload();
    });

    // SIGWINCH — gracefully stop workers only (keep custom + helper alive).
    // Mirrors v1: SignalToProcess(ptWorker, SIG_SHUTDOWN).
    loop.add_signal(SIGWINCH, [this](const signalfd_siginfo&) {
        logger_->notice("SIGWINCH received — gracefully stopping workers");
        for (auto& child : children_)
        {
            if (child.role == ProcessRole::worker)
            {
                child.shutting_down = true;
                ::kill(child.pid, SIGQUIT);
            }
        }
    });

    // SIGUSR1 — reopen logs, forward to all children
    loop.add_signal(SIGUSR1, [this](const signalfd_siginfo&) {
        logger_->notice("SIGUSR1 received — reopening logs, forwarding to children");
        logger_->reopen();
        for (auto& child : children_)
            ::kill(child.pid, SIGUSR1);
    });

    // Reap any children that died during the spawn→eventloop window
    // (their SIGCHLD was lost before signalfd was set up).
    reap_children();

    loop.run();

    logger_->notice("{} master process exiting", name_);
}

// ─── Single process loop ─────────────────────────────────────────────────────

void Application::single_run()
{
    // Re-arm the alternate signal stack: POSIX does not inherit sigaltstack across fork().
    setup_crash_altstack();

    logger_->notice("{} single process started (pid={})", name_, ::getpid());
    set_limit_nofile(cfg_limit_nofile());

    EventLoop loop;
    worker_loop_ = &loop;

    loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGINT,  [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGHUP, [this](const signalfd_siginfo&) {
        logger_->notice("SIGHUP — reconfiguring");
        load_config();
        on_reload();
    });
    loop.add_signal(SIGUSR1, [this](const signalfd_siginfo&) {
        logger_->notice("SIGUSR1 — reopening logs");
        logger_->reopen();
#ifdef WITH_POSTGRESQL
        if (pg_logger_) pg_logger_->reopen();
#endif
        if (stream_logger_) stream_logger_->reopen();
    });

    try
    {
        on_worker_start(loop);
        module_manager_.on_start();
    }
    catch (const std::exception& e)
    {
        logger_->error("{} startup failed: {}", name_, e.what());
        exit_code_ = 1;
        return;
    }

    // Drop privileges after initialization (sockets bound, DB connected)
    set_user(cfg_user(), cfg_group());

    // Set process title AFTER modules are registered
    auto names = module_manager_.module_names();
    set_process_title(names.empty()
        ? fmt::format("{}: single process {}", name_, cmdline_)
        : fmt::format("{}: single process ({})", name_, names));

    loop.run();

    module_manager_.on_stop();
    stop_db();
    logger_->notice("{} single process exiting (pid={})", name_, ::getpid());
}

// ─── Worker / helper loops ───────────────────────────────────────────────────

void Application::worker_run()
{
    // Unblock signals inherited from master (master blocks them for signalfd)
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    // Re-arm the alternate signal stack: POSIX does not inherit sigaltstack across fork().
    setup_crash_altstack();

    logger_->notice("{} worker process started (pid={})", name_, ::getpid());
    set_limit_nofile(cfg_limit_nofile());

    EventLoop loop;
    worker_loop_ = &loop;

    loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGUSR1, [this](const signalfd_siginfo&) {
        logger_->notice("SIGUSR1 — worker reopening logs");
        logger_->reopen();
#ifdef WITH_POSTGRESQL
        if (pg_logger_) pg_logger_->reopen();
#endif
        if (stream_logger_) stream_logger_->reopen();
    });

    try
    {
        on_worker_start(loop);
        module_manager_.on_start();
    }
    catch (const std::exception& e)
    {
        logger_->error("{} worker startup failed: {}", name_, e.what());
        exit_code_ = 1;
        return;
    }

    // Drop privileges after initialization (sockets bound, DB connected)
    set_user(cfg_user(), cfg_group());

    // Set process title AFTER modules are registered
    auto names = module_manager_.module_names();
    set_process_title(names.empty()
        ? fmt::format("{}: worker process", name_)
        : fmt::format("{}: worker process ({})", name_, names));

    loop.run();

    module_manager_.on_stop();
    stop_db();
    logger_->notice("{} worker process exiting (pid={})", name_, ::getpid());
}

void Application::helper_run()
{
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    // Re-arm the alternate signal stack: POSIX does not inherit sigaltstack across fork().
    setup_crash_altstack();

    logger_->notice("{} helper process started (pid={})", name_, ::getpid());
    set_limit_nofile(cfg_limit_nofile());

    EventLoop loop;

    loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGUSR1, [this](const signalfd_siginfo&) {
        logger_->notice("SIGUSR1 — helper reopening logs");
        logger_->reopen();
#ifdef WITH_POSTGRESQL
        if (pg_logger_) pg_logger_->reopen();
#endif
    });

    try
    {
        on_helper_start(loop);
        module_manager_.on_start();
    }
    catch (const std::exception& e)
    {
        logger_->error("{} helper startup failed: {}", name_, e.what());
        exit_code_ = 1;
        return;
    }

    // Drop privileges after initialization (DB connected)
    set_user(cfg_user(), cfg_group());

    // Set process title AFTER modules are registered
    auto names = module_manager_.module_names();
    set_process_title(names.empty()
        ? fmt::format("{}: helper process", name_)
        : fmt::format("{}: helper process ({})", name_, names));

    // Module heartbeat — every 1 second (mirrors worker's heartbeat in start_http_server)
    loop.add_timer(std::chrono::seconds(1),
        [this]
        {
            module_manager_.heartbeat(std::chrono::system_clock::now());
        });

#ifdef WITH_POSTGRESQL
    // PgPool heartbeat — every 60 seconds (connection health check + reconnect)
    if (db_pool_) {
        loop.add_timer(std::chrono::seconds(60),
            [this]
            {
                db_pool_->heartbeat();
            });
    }
#endif

    loop.run();

    module_manager_.on_stop();
    stop_db();
    logger_->notice("{} helper process exiting (pid={})", name_, ::getpid());
}

// ─── Custom process loop ──────────────────────────────────────────────────

#ifdef WITH_POSTGRESQL

void Application::custom_process_run(CustomProcess& proc)
{
    // 1. Signal unblock (inherited from master's signalfd mask)
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    // 2. Crash handler (POSIX does not inherit sigaltstack across fork)
    setup_crash_altstack();

    // 3. Limits + log
    logger_->notice("{} process '{}' started (pid={})", name_, proc.name(), ::getpid());
    set_limit_nofile(cfg_limit_nofile());

    // 4. EventLoop + signal handlers
    EventLoop loop;

    loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
    loop.add_signal(SIGUSR1, [this](const signalfd_siginfo&) {
        logger_->notice("SIGUSR1 — process reopening logs");
        logger_->reopen();
        if (pg_logger_) pg_logger_->reopen();
    });

    // 5. PgPool (if helper conninfo is configured)
    const auto& conninfo = settings().pg_conninfo_helper;
    if (!conninfo.empty()) {
        setup_db(loop, conninfo,
            static_cast<std::size_t>(settings().pg_pool_min),
            static_cast<std::size_t>(settings().pg_pool_max));
    }

    // 6. Process on_start
    try {
        proc.on_start(loop, *this);
    } catch (const std::exception& e) {
        logger_->error("{} process '{}' startup failed: {}",
                       name_, proc.name(), e.what());
        stop_db();
        exit_code_ = 1;
        return;
    }

    // 7. Drop privileges after initialization (sockets bound, DB connected)
    set_user(cfg_user(), cfg_group());

    // Set process title AFTER on_start (modules may be registered)
    auto names = module_manager_.module_names();
    set_process_title(names.empty()
        ? fmt::format("{}: {} process", name_, proc.name())
        : fmt::format("{}: {} process ({})", name_, proc.name(), names));

    // 7. Heartbeat timer (1s)
    loop.add_timer(std::chrono::seconds(1),
        [&proc] {
            proc.heartbeat(std::chrono::system_clock::now());
        });

    // 8. Event loop
    loop.run();

    // 9. Cleanup: on_stop() first, then stop_db() while EventLoop is still alive
    proc.on_stop();
    stop_db();
    logger_->notice("{} process '{}' exiting (pid={})",
                    name_, proc.name(), ::getpid());
}

#endif // WITH_POSTGRESQL

// ─── Process spawning ────────────────────────────────────────────────────────

pid_t Application::fork_child(ProcessRole role, std::string child_name,
                               std::function<void()> custom_fn)
{
    pid_t pid = ::fork();
    if (pid < 0)
        throw std::system_error(errno, std::system_category(), "fork");

    if (pid == 0)
    {
        // ── Child process ──────────────────────────────────────────────────
        role_ = role;

        if (role == ProcessRole::worker)
            worker_run();
        else if (role == ProcessRole::helper)
            helper_run();
        else if (role == ProcessRole::custom && custom_fn)
            custom_fn();

        std::exit(0);
    }

    // ── Parent (master) ────────────────────────────────────────────────────
    logger_->notice("spawned {} '{}' pid={}", role_name(role), child_name, pid);
    children_.push_back({pid, role, std::move(child_name), false});
    return pid;
}

void Application::spawn_workers()
{
    int count = settings_.effective_workers();
    for (int i = 0; i < count; ++i)
        fork_child(ProcessRole::worker, fmt::format("worker#{}", i + 1));
}

void Application::spawn_helper()
{
    fork_child(ProcessRole::helper, "helper");
}

// ─── Reaping children ────────────────────────────────────────────────────────

void Application::reap_children()
{
    int status = 0;
    pid_t pid;

    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0)
    {
        auto it = std::find_if(children_.begin(), children_.end(),
            [pid](const ChildInfo& c) { return c.pid == pid; });

        if (it == children_.end())
        {
            logger_->warn("reaped unknown child pid={}", pid);
            continue;
        }

        if (WIFEXITED(status))
            logger_->notice("{} '{}' (pid={}) exited with code {}",
                role_name(it->role), it->name, pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            logger_->warn("{} '{}' (pid={}) killed by signal {}",
                role_name(it->role), it->name, pid, WTERMSIG(status));

        bool respawn = !it->shutting_down && !shutting_down_;
        ProcessRole respawn_role = it->role;
        std::string respawn_name = it->name;

        children_.erase(it);

        if (respawn)
        {
            // ── Respawn rate limiting ────────────────────────────────────────
            // Prevent tight crash loops: if a child exits too quickly, delay
            // the respawn with exponential backoff (1s → 2s → 4s, max 30s).
            // Resets after 60s of stable running.
            auto now = std::chrono::steady_clock::now();
            if (now - last_respawn_time_ < std::chrono::seconds(2))
            {
                ++rapid_respawn_count_;
                if (rapid_respawn_count_ > 3)
                {
                    auto delay = std::min(1 << (rapid_respawn_count_ - 3), 30);
                    logger_->warn("rapid respawn detected ({} in a row) — delaying {} '{}' by {}s",
                                  rapid_respawn_count_, role_name(respawn_role), respawn_name, delay);
                    ::sleep(static_cast<unsigned>(delay));
                }
            }
            else if (now - last_respawn_time_ > std::chrono::seconds(60))
            {
                rapid_respawn_count_ = 0;
            }
            last_respawn_time_ = now;

            logger_->notice("respawning {} '{}'", role_name(respawn_role), respawn_name);

            if (respawn_role == ProcessRole::custom)
            {
                // Find the CustomProcessEntry by name and re-create the lambda
                // so that custom_process_run() is called in the child.
                bool found = false;
                for (auto& cp : custom_processes_) {
                    if (cp.name == respawn_name) {
#ifdef WITH_POSTGRESQL
                        fork_child(respawn_role, std::move(respawn_name),
                            [this, &cp] { custom_process_run(*cp.process); });
#else
                        auto fn = cp.fn;
                        fork_child(respawn_role, std::move(respawn_name),
                            [this, fn] {
                                EventLoop loop;
                                loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
                                loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
                                fn(loop);
                                loop.run();
                            });
#endif
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    logger_->error("cannot respawn custom '{}': entry not found", respawn_name);
                }
            }
            else
            {
                fork_child(respawn_role, std::move(respawn_name));
            }
        }
    }
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

void Application::fast_shutdown()
{
    logger_->notice("fast shutdown requested");
    shutting_down_ = true;

    // Proactively reap any zombie children first (they won't generate new SIGCHLD).
    reap_children();

    for (auto& child : children_)
    {
        child.shutting_down = true;
        ::kill(child.pid, SIGTERM);
    }
    // Do NOT clear children_ here: SIGCHLD handler reaps them one-by-one
    // and stops the master loop once all are gone (mirrors v1 ReapChildren path).
}

void Application::graceful_shutdown()
{
    logger_->notice("graceful shutdown requested");
    shutting_down_ = true;
    graceful_ = true;

    // Proactively reap any zombie children first (they won't generate new SIGCHLD).
    reap_children();

    for (auto& child : children_)
    {
        child.shutting_down = true;
        ::kill(child.pid, SIGQUIT);
    }
    // The master loop will call loop.stop() once children_ is empty (via SIGCHLD handler)
}

// ─── Rolling restart (SIGHUP) ────────────────────────────────────────────────

void Application::rolling_restart()
{
    // Reload config and re-populate settings
    try
    {
        config_ = std::make_unique<Config>(Config::from_file(config_file_));
        settings_.populate(*config_);
    }
    catch (const ConfigError& e)
    {
        logger_->error("config reload failed: {} — keeping old config", e.what());
        return;
    }

    // Re-apply locale after config reload
    if (!settings_.locale.empty())
        if (::setlocale(LC_ALL, settings_.locale.c_str()) == nullptr)
            logger_->warn("setlocale('{}') failed on reload", settings_.locale);

    // Mark old workers and helpers for retirement
    // (custom processes are also restarted — mirrors v1 StartCustomProcesses(JUST_RESPAWN))
    for (auto& child : children_)
        child.shutting_down = true;

    // Spawn fresh custom processes
    for (auto& cp : custom_processes_)
    {
#ifdef WITH_POSTGRESQL
        fork_child(ProcessRole::custom, cp.name, [this, &cp] {
            custom_process_run(*cp.process);
        });
#else
        auto fn = cp.fn;
        fork_child(ProcessRole::custom, cp.name, [this, fn] {
            EventLoop loop;
            loop.add_signal(SIGTERM, [&loop](const signalfd_siginfo&) { loop.stop(); });
            loop.add_signal(SIGQUIT, [&loop](const signalfd_siginfo&) { loop.stop(); });
            fn(loop);
            loop.run();
        });
#endif
    }

    // Spawn fresh helper (if configured)
    if (cfg_helper()) spawn_helper();

    // Spawn fresh workers
    spawn_workers();

    // Allow new processes a moment to start before old ones stop
    // (mirrors v1 usleep(100 * 1000) in CProcessMaster::Run sig_reconfigure branch)
    ::usleep(100 * 1000);

    // Then tell old processes to exit gracefully
    for (auto& child : children_)
    {
        if (child.shutting_down)
            ::kill(child.pid, SIGQUIT);
    }
}

// ─── OS-level helpers ─────────────────────────────────────────────────────────

void Application::init_setproctitle(int argc, char* argv[])
{
    os_argc_ = argc;
    os_argv_ = argv;

    // Calculate contiguous argv+environ memory extent
    os_argv_last_ = os_argv_[0];
    for (int i = 0; i < os_argc_; ++i) {
        if (os_argv_last_ == os_argv_[i])
            os_argv_last_ = os_argv_[i] + std::strlen(os_argv_[i]) + 1;
    }

    // Copy environ to heap, extend os_argv_last_ through contiguous environ entries
    std::size_t env_size = 0;
    for (int i = 0; environ[i]; ++i)
        env_size += std::strlen(environ[i]) + 1;

    if (env_size > 0) {
        os_environ_ = new char[env_size];
        char* dst = os_environ_;

        for (int i = 0; environ[i]; ++i) {
            std::size_t len = std::strlen(environ[i]) + 1;
            if (os_argv_last_ == environ[i]) {
                os_argv_last_ = environ[i] + len;
            }
            std::memcpy(dst, environ[i], len);
            environ[i] = dst;
            dst += len;
        }
        os_argv_last_--;  // point to last usable byte
    }
}

void Application::set_process_title(std::string_view title)
{
    // 1. prctl — sets thread name (/proc/pid/comm, max 15 chars)
    std::string short_name(title.substr(0, 15));
    ::prctl(PR_SET_NAME, short_name.c_str(), 0, 0, 0);

    // 2. argv[0] rewrite — sets full title (/proc/pid/cmdline, htop/ps)
    if (!os_argv_ || !os_argv_last_)
        return;

    os_argv_[1] = nullptr;

    auto max_len = static_cast<std::size_t>(os_argv_last_ - os_argv_[0]);
    auto copy_len = std::min(title.size(), max_len);

    std::memcpy(os_argv_[0], title.data(), copy_len);

    // Pad remaining space with NUL
    if (copy_len < max_len)
        std::memset(os_argv_[0] + copy_len, '\0', max_len - copy_len);
}

void Application::set_limit_nofile(std::uint32_t limit)
{
    if (limit == 0)
        return;
    struct rlimit rl{limit, limit};
    if (::setrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        // logger may not be available in all contexts; use stderr fallback
        std::fprintf(stderr, "setrlimit(RLIMIT_NOFILE, %u): %s\n", limit, std::strerror(errno));
    }
}

void Application::set_user(std::string_view user, std::string_view group)
{
    if (user.empty())
        return;

    // Look up group first (if specified)
    if (!group.empty())
    {
        errno = 0;
        struct group* grp = ::getgrnam(std::string(group).c_str());
        if (!grp)
        {
            std::fprintf(stderr, "getgrnam('%s'): %s\n",
                std::string(group).c_str(),
                errno ? std::strerror(errno) : "group not found");
            return;
        }
        if (::setgid(grp->gr_gid) == -1)
        {
            std::fprintf(stderr, "setgid(%u): %s\n", grp->gr_gid, std::strerror(errno));
            return;
        }
    }

    errno = 0;
    struct passwd* pw = ::getpwnam(std::string(user).c_str());
    if (!pw)
    {
        std::fprintf(stderr, "getpwnam('%s'): %s\n",
            std::string(user).c_str(),
            errno ? std::strerror(errno) : "user not found");
        return;
    }

    if (group.empty() && ::setgid(pw->pw_gid) == -1)
    {
        std::fprintf(stderr, "setgid(%u): %s\n", pw->pw_gid, std::strerror(errno));
        return;
    }

    // Initialize supplementary groups (required before setuid for proper group access).
    // By this point setgid() has already been called with the correct gid.
    if (::initgroups(pw->pw_name, ::getgid()) == -1)
    {
        std::fprintf(stderr, "initgroups('%s', %u): %s\n",
            pw->pw_name, ::getgid(), std::strerror(errno));
        return;
    }

    if (::setuid(pw->pw_uid) == -1)
        std::fprintf(stderr, "setuid(%u): %s\n", pw->pw_uid, std::strerror(errno));
}

// ─── Directories ──────────────────────────────────────────────────────────────

void Application::create_directories()
{
    if (settings_.prefix.empty())
        return;

    namespace fs = std::filesystem;
    std::error_code ec;

    // Always create directories needed for PID and log files
    if (!settings_.pid_file.empty())
        fs::create_directories(settings_.pid_file.parent_path(), ec);
    if (!settings_.error_log.empty())
        fs::create_directories(settings_.error_log.parent_path(), ec);

    // Standard prefix subdirectories — from config "directories" array or defaults
    std::vector<std::string> dirs{"logs", "conf", "cache"};
    if (config_ && config_->has("directories"))
    {
        auto& arr = config_->json()["directories"];
        if (arr.is_array())
        {
            dirs.clear();
            for (auto& d : arr)
                if (d.is_string())
                    dirs.push_back(d.get<std::string>());
        }
    }

    const fs::path p(settings_.prefix);
    for (const auto& dir : dirs)
        fs::create_directories(p / dir, ec);
}

// ─── Custom processes ─────────────────────────────────────────────────────────

#ifdef WITH_POSTGRESQL
void Application::add_custom_process(std::unique_ptr<CustomProcess> proc)
{
    auto name = std::string(proc->name());
    custom_processes_.push_back({std::move(name), std::move(proc)});
}

void Application::add_custom_process(std::unique_ptr<ProcessModule> mod)
{
    add_custom_process(std::make_unique<ModuleProcess>(std::move(mod)));
}
#else
void Application::add_custom_process(std::string name, std::function<void(EventLoop&)> fn)
{
    custom_processes_.push_back({std::move(name), std::move(fn)});
}
#endif

#ifdef WITH_POSTGRESQL

// ─── PostgreSQL pool ─────────────────────────────────────────────────────────

PgPool& Application::setup_db(EventLoop& loop, std::string conninfo,
                               std::size_t min_conns, std::size_t max_conns)
{
    // Create dedicated postgres logger (writes to stderr + separate postgres.log)
    {
        pg_logger_ = std::make_unique<Logger>();
        pg_logger_->set_level(LogLevel::debug);
        pg_logger_->add_target(std::make_unique<StderrTarget>());

        if (!settings_.postgres_log.empty()) {
            try {
                std::filesystem::create_directories(settings_.postgres_log.parent_path());
                pg_logger_->set_file_target(settings_.postgres_log.string(), settings_.log_max_size,
                                             settings_.log_keep_rotated, settings_.log_compress);
            } catch (const std::exception& e) {
                logger_->warn("cannot open postgres log '{}': {}",
                    settings_.postgres_log.string(), e.what());
            }
        }
    }

    db_pool_ = std::make_unique<PgPool>(loop, std::move(conninfo),
        min_conns, max_conns, pg_logger_.get());
    db_pool_->start();
    return *db_pool_;
}

PgPool& Application::db_pool()
{
    if (!db_pool_)
        throw std::logic_error("setup_db() must be called before db_pool()");
    return *db_pool_;
}

#endif // WITH_POSTGRESQL

// ─── Stream logger ───────────────────────────────────────────────────────────

Logger& Application::stream_logger()
{
    if (!stream_logger_) {
        stream_logger_ = std::make_unique<Logger>();
        stream_logger_->set_level(LogLevel::debug);
        stream_logger_->add_target(std::make_unique<StderrTarget>());

        auto stream_log = settings_.resolve(APP_STREAM_LOG_FILE);
        if (!stream_log.empty()) {
            try {
                std::filesystem::create_directories(stream_log.parent_path());
                stream_logger_->set_file_target(stream_log.string(), settings_.log_max_size,
                    settings_.log_keep_rotated, settings_.log_compress);
            } catch (const std::exception& e) {
                logger_->warn("cannot open stream log '{}': {}",
                    stream_log.string(), e.what());
            }
        }
    }
    return *stream_logger_;
}

// ─── WebSocket handler ────────────────────────────────────────────────────────

void Application::set_ws_handler(WsHandler h)
{
    ws_handler_ = std::move(h);
}

// ─── HTTP server ─────────────────────────────────────────────────────────────

void Application::start_http_server(EventLoop& loop, uint16_t port)
{
    worker_loop_ = &loop;

    std::shared_ptr<TcpListener> listener;
    if (listen_fd_ >= 0) {
        // Worker: borrow fd inherited from master (no bind, no close on exit)
        listener = std::make_shared<TcpListener>(TcpListener::borrow_fd(listen_fd_));
    } else {
        // Single-process mode: create own listener
        listener = std::make_shared<TcpListener>(port, settings_.server_backlog);
    }
    http_port_ = listener->local_port();

    logger_->notice("HTTP server listening on port {}", http_port_);

    // Accept loop
    loop.add_io(listener->fd(), EPOLLIN,
        [this, &loop, listener](uint32_t)
        {
            auto conn_opt = listener->accept();
            if (!conn_opt)
                return;

            auto http_conn = std::make_shared<HttpConnection>(std::move(*conn_opt), &loop);
            int  conn_fd   = http_conn->fd();

            loop.add_io(conn_fd, EPOLLIN | EPOLLRDHUP,
                [this, &loop, http_conn, conn_fd](uint32_t events)
                {
                    // Drain pending async writes (sendfile, buffered responses)
                    if (events & EPOLLOUT)
                        http_conn->on_writable();

                    if (!(events & (EPOLLIN | EPOLLRDHUP)))
                        return;

                    bool upgraded = false;

                    bool keep = http_conn->on_readable(
                        [this, &loop, &http_conn, conn_fd, &upgraded]
                        (const HttpRequest& req, HttpResponse& resp)
                        {
                            // Expose the connection handle so async modules
                            // (e.g. PGHTTP) can send deferred responses later.
                            req.connection_ctx = http_conn;

                            // WebSocket upgrade?
                            if (ws_handler_ && is_ws_upgrade(req))
                            {
                                auto ws_opt = ws_upgrade(*http_conn, req);
                                if (ws_opt)
                                {
                                    upgraded = true;
                                    ws_handler_(loop, std::move(*ws_opt), req);
                                }
                                else
                                {
                                    resp.set_status(400, "Bad Request")
                                        .set_body("WebSocket upgrade failed");
                                }
                                return;
                            }

                            // Normal HTTP dispatch
                            if (!module_manager_.execute(req, resp))
                                resp.set_status(404, "Not Found")
                                    .set_body("404 Not Found");
                        });

                    if (!keep && !upgraded)
                        loop.remove_io(conn_fd);
                });
        });

    // Module heartbeat — every 1 second
    loop.add_timer(std::chrono::seconds(1),
        [this]
        {
            module_manager_.heartbeat(std::chrono::system_clock::now());
        });

#ifdef WITH_POSTGRESQL
    // PgPool heartbeat — every 60 seconds (connection health check + reconnect)
    if (db_pool_) {
        loop.add_timer(std::chrono::seconds(60),
            [this]
            {
                db_pool_->heartbeat();
            });
    }
#endif
}

} // namespace apostol
