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

// ─── safe_log_level ──────────────────────────────────────────────────────────
//
// level_from_string throws on an unknown or empty value, and AppSettings::validate()
// reports a bad log.level without correcting it. A typo there must not take down a
// worker that the master will then respawn forever.

static LogLevel safe_log_level(const std::string& name)
{
    try {
        return level_from_string(name);
    } catch (const std::exception&) {
        return LogLevel::notice;
    }
}


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

    // The base every configuration file is applied onto — the build's
    // defaults, set_info() and -p, before any file (see base_settings_).
    base_settings_ = settings_;

    // A configuration this process cannot use is the operator's error, and the
    // answer to it is a message and a non-zero exit — not a core dump. Without
    // this catch the ConfigError load_config() throws reached no handler at all:
    // std::terminate, SIGABRT, rc 134 and a memory image written to disk — on
    // every start of a misconfigured service, and on every -t, whose whole
    // purpose is to report that verdict BY exit code. So -t could report success
    // and nothing else: a parse error, an empty file and a config that parses but
    // fails validate() all aborted alike.
    //
    // Reporting the failure belongs here rather than in load_config(), because
    // only the caller knows what was being attempted: a start that will not
    // happen, or a reload that changes nothing (single_run's SIGHUP below).
    // load_config() therefore logs the per-key validation errors, which name
    // things this line cannot, and leaves the verdict to whoever asked for it.
    //
    // ConfigError only. Anything else escaping load_config() is this library's
    // fault rather than the operator's, and for that a crash is still the honest
    // answer — unlike the catch (std::exception) further down around
    // start_process(), which stands after the configuration is known good and
    // where any failure left IS the run failing.
    try
    {
        load_config();
    }
    catch (const ConfigError& e)
    {
        if (test_config_)
            logger_->error("configuration '{}' test failed: {}",
                config_file_.string(), e.what());
        else
            logger_->error("configuration '{}' cannot be used: {}",
                config_file_.string(), e.what());
        return 1;
    }

    // Install crash handler now that we know the error log path.
    // Re-registers in every child process too (see worker_run/helper_run/single_run).
    install_crash_handler(settings_.error_log.string());

    // CLI -w and -l, and the locale itself, are applied with the configuration
    // (read_config / apply_config), so that a reload does the same.

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
                settings_.server_port, settings_.server_backlog, settings_.server_listen);
            listen_fd_ = master_listener_->fd();
            http_port_ = master_listener_->local_port();
            logger_->notice("master: bound listening socket on {}:{}",
                            settings_.server_listen.empty() ? "*" : settings_.server_listen,
                            http_port_);
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
            config_explicit_ = true;
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

Application::StagedConfig Application::read_config(bool reload) const
{
    // Nothing of this process changes until the whole configuration is in hand.
    // Reading into locals is what lets a caller say "keeping old config" and be
    // telling the truth: parsing is not the last step that can fail, and it is
    // not even the likely one. AppSettings::populate() throws ConfigError out of
    // the typed getters on a value of the wrong type ("workers": "abc", a
    // ${VAR} substitution that did not resolve to a number) and assigns field by
    // field, so populating the live settings_ in place left it half-new on the
    // way out — and its first lines recompute every path under prefix. A reload
    // that was refused would already have moved the pid file and the logs.
    //
    // One reading for all three callers — a start, the master's SIGHUP and the
    // single process's SIGHUP — so that one file gets one verdict. They used to
    // differ: the master's reload skipped validate() and never reread oauth2/
    // or sites/ (new workers forked with the old providers), and a missing file
    // was a refusal to the master and "defaults" to the single process.
    StagedConfig staged;

    std::error_code ec;
    const bool present = std::filesystem::exists(config_file_, ec);
    if (ec)
        throw ConfigError(fmt::format("cannot reach config file '{}': {}",
            config_file_.string(), ec.message()));

    if (present && std::filesystem::is_directory(config_file_, ec))
        throw ConfigError(fmt::format("config file '{}' is a directory",
            config_file_.string()));

    if (!present)
    {
        // A reload means "apply the file": with no file there is nothing to
        // apply, and falling back to built-in defaults would swap a working
        // configuration for one nobody wrote. Nor when the file was named with
        // -c — a typo there is the operator's error, and -t must be able to
        // say so. Only an application run without -c, whose default path is
        // absent, starts on defaults.
        if (reload || config_explicit_)
            throw ConfigError(fmt::format("config file '{}' not found",
                config_file_.string()));

        logger_->notice("config file '{}' not found, using defaults",
            config_file_.string());
        staged.config = std::make_unique<Config>(Config::from_string("{}"));
    }
    else
    {
        staged.config = std::make_unique<Config>(Config::from_file(config_file_));
    }

    // Populate a copy — see above — of the base: the build's defaults and -p,
    // not what is in force now. From what is in force, a key removed from the
    // file kept its old value on a reload while a start would give it the
    // default: one file, two results.
    staged.settings = base_settings_;
    staged.settings.populate(*staged.config);

    // Strict validation — collect ALL errors, log each (they name keys the
    // caller's one line cannot), then refuse. The same verdict for -t and for a
    // start: -t exists to predict the start, and a start used to go on after
    // "applying defaults for invalid values" — which it never did; a zero
    // port or 300 workers went into service as written.
    // The command line over the file — CLI -w over "workers" (mirrors v1
    // DefaultCommands: skip config if CLI set), -l over "locale" — before the
    // check, so that what is checked is what the process will run with:
    // applied after it, -w 1000 went past the limit of 256 that the same
    // number in the file would not. And on every reading, not only at start:
    // a reload of a file carrying the key took the flag away — a container
    // started with -w $WORKER_PROCESSES came back from SIGHUP with the file's
    // number of workers.
    if (cli_workers_ >= 0)
        staged.settings.workers = cli_workers_;
    if (!locale_.empty())
        staged.settings.locale = locale_;

    auto errors = staged.settings.validate();
    if (!errors.empty())
    {
        for (auto& e : errors)
            logger_->error("configuration error [{}]: {}", e.key, e.message);

        throw ConfigError(fmt::format("configuration has {} error(s)", errors.size()));
    }

    // The directories next to the prefix. An unreadable one threw
    // filesystem_error out of directory_iterator — past every ConfigError
    // handler, so a start ended in std::terminate and a core, and a reload in
    // a dead master.
    try
    {
        staged.providers.load(staged.settings.resolve("oauth2"));
        staged.sites.load(staged.settings.resolve("sites"));
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        throw ConfigError(e.what());
    }

    // A file the loaders passed over is not a refusal — skipping is their
    // contract — but it is never silent: a provider that disappears over a typo
    // turns every token of its audience into "unknown audience", and now that a
    // reload rereads these directories, that can happen to a running service.
    for (const auto& why : staged.providers.skipped())
        logger_->warn("oauth2 file skipped: {}", why);
    for (const auto& why : staged.sites.skipped())
        logger_->warn("sites file skipped: {}", why);

    return staged;
}

void Application::load_config(bool reload)
{
    apply_config(read_config(reload));
}

void Application::apply_config(StagedConfig&& staged)
{
    // Accepted: all of it at once, and only now is anything of this process
    // touched. providers_ and sites_ are assigned, not rebuilt: modules hold
    // references to these members, not copies.
    config_    = std::move(staged.config);
    settings_  = std::move(staged.settings);
    providers_ = std::move(staged.providers);
    sites_     = std::move(staged.sites);

    // The locale into effect — here, so that a start and both reloads do it
    // alike (the single process's SIGHUP never did). Mirrors v1
    // DefaultLocale.SetLocale.
    if (!settings_.locale.empty())
        if (::setlocale(LC_ALL, settings_.locale.c_str()) == nullptr)
            logger_->warn("setlocale('{}') failed — using system default locale",
                settings_.locale);

    // Apply log level from settings
    try
    {
        logger_->set_level(level_from_string(settings_.log_level));

        // The service loggers are created once, so a reload has to reach them too;
        // otherwise raising log.level and sending SIGHUP silently does nothing for
        // PostgreSQL and stream diagnostics. A dedicated postgres.log stays at
        // debug by design — see setup_db().
        if (stream_logger_)
            stream_logger_->set_level(safe_log_level(settings_.log_level));
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

    // Visible to reap_children() so a throttled respawn is scheduled on this loop
    // rather than run under a blocking ::sleep(). Cleared before the loop is
    // destroyed at the end of this function.
    master_loop_ = &loop;

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

    // SIGHUP — reload config + rolling restart. on_reload() only when the new
    // configuration was taken: after a refusal the master keeps the old one and
    // restarts nothing, so telling the application to reconfigure would have it
    // act on a file that never took effect. Same rule as the single-process
    // branch below.
    loop.add_signal(SIGHUP, [this](const signalfd_siginfo&) {
        logger_->notice("SIGHUP received — reloading config");
        if (rolling_restart())
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

    master_loop_ = nullptr;

    logger_->notice("{} master process exiting", name_);
}

// ─── Single process loop ─────────────────────────────────────────────────────

#ifdef WITH_POSTGRESQL

// ─── drain_db ────────────────────────────────────────────────────────────────
//
// on_stop() runs after loop.run() has returned, so anything a module queues there —
// a service session to close, a last write — sits in a queue nobody pumps again.
// Give those queries a bounded chance to finish before the pool is destroyed.
//
// Bounded on purpose: a database that has stopped answering must not hold the
// process from exiting. The drain ends as soon as nothing is outstanding, so a
// healthy shutdown costs one round trip rather than the timeout.

void Application::drain_db(EventLoop& loop)
{
    // Every pool, not just the default one: a module may have been handed a named
    // pool (FileServer runs on "helper"), and its session closes through that.
    auto outstanding = [this] {
        std::size_t n = db_pool_ ? db_pool_->outstanding() : 0;
        for (const auto& [name, pool] : named_pools_)
            if (pool)
                n += pool->outstanding();
        return n;
    };

    if (outstanding() == 0)
        return;

    bool drained = false;

    try {
        drained = loop.run_for(k_shutdown_drain,
                               [&outstanding] { return outstanding() == 0; });
    } catch (const std::exception& e) {
        // Best effort by definition. An exception here must not skip stop_db().
        if (logger_)
            logger_->warn("{} shutdown drain failed: {}", name_, e.what());
        return;
    }

    if (!drained && logger_)
        logger_->warn("{} shutdown: {} database queries did not complete in {} ms",
                      name_, outstanding(),
                      static_cast<long long>(k_shutdown_drain.count()));
}

#endif // WITH_POSTGRESQL

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
        // A reload that cannot be read leaves the process running on what it
        // already has. The ConfigError went uncaught out of a signal callback on
        // the loop, so reloading a broken file killed a WORKING single-process
        // instance: std::terminate, SIGABRT, and the service gone over an edit
        // that never took effect. "Keeping old config" is exact, not a figure of
        // speech: load_config() builds the new configuration in locals and swaps
        // both it and the settings in one step at the end, so a refusal leaves
        // nothing half-applied. on_reload() is skipped with it, because nothing
        // was reconfigured.
        try
        {
            load_config(/*reload=*/true);
        }
        catch (const ConfigError& e)
        {
            logger_->error("config reload failed: {} — keeping old config", e.what());
            return;
        }
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
        // Same order as the normal exit below: the pool goes while the loop is
        // alive and before ~Application. Left to ~Application it logged
        // "Disconnected" into pg_logger_, already destroyed by then, and the
        // process sat on the dead logger's mutex with every signal blocked
        // for signalfd — only kill -9 ended it (T281).
        stop_db();
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
    drain_db(loop);
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
        stop_db();   // while the loop is alive, before ~Application — see single_run
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
    drain_db(loop);
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
        stop_db();   // while the loop is alive, before ~Application — see single_run
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
                for (auto& [_, pool] : named_pools_)
                    pool->heartbeat();
            });
    }
#endif

    loop.run();

    module_manager_.on_stop();
    drain_db(loop);
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
        ? fmt::format("{}: {} process", name_, proc.title())
        : fmt::format("{}: {} process ({})", name_, proc.title(), names));

    // 7. Heartbeat timer (1s). The id is kept because the drain below runs the loop
    // again after on_stop(): a process whose session was released there would be
    // asked to beat once more and would log back in, leaving behind the very
    // session the drain exists to close. Modules are latched by ModuleManager;
    // a custom process has no manager, so its timer is cancelled by hand.
    const auto heartbeat_timer = loop.add_timer(std::chrono::seconds(1),
        [&proc] {
            proc.heartbeat(std::chrono::system_clock::now());
        });

    // 7b. PgPool heartbeat — every 60 seconds, mirroring worker_run() and
    // helper_run(). A custom process is its own OS process with its own pool
    // and its own LISTEN connection, and this timer was simply absent here:
    // nothing in the process ever checked connection health. MessageServer
    // ("outbox") and ReportServer ("report") therefore had no way at all to
    // notice a lost subscription — the recovery path in PgPool existed but
    // had nothing to drive it. Cancelled next to the one above, and for the
    // same reason: the drain re-runs the loop after on_stop().
    const auto pool_heartbeat_timer = db_pool_
        ? loop.add_timer(std::chrono::seconds(60),
            [this] {
                db_pool_->heartbeat();
                for (auto& [_, pool] : named_pools_)
                    pool->heartbeat();
            })
        : EventLoop::kInvalidTimer;

    // 8. Event loop
    loop.run();

    // 9. Cleanup: on_stop() first, then stop_db() while EventLoop is still alive
    proc.on_stop();
    loop.cancel_timer(heartbeat_timer);
    if (pool_heartbeat_timer != EventLoop::kInvalidTimer)
        loop.cancel_timer(pool_heartbeat_timer);
    drain_db(loop);
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
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);

        role_ = role;

        if (role == ProcessRole::worker)
            worker_run();
        else if (role == ProcessRole::helper)
            helper_run();
        else if (role == ProcessRole::custom && custom_fn)
            custom_fn();

        // exit_code_, not a literal 0: a child whose startup threw sets exit_code_
        // to 1 and returns here, and exiting 0 anyway reported a failed start to the
        // master — and to anyone watching exit codes, the first thing a human or a
        // monitor checks — as a clean exit. The master then logs "exited with code 0"
        // for what was a failure.
        std::exit(exit_code_);
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

        bool respawn = !it->shutting_down && !shutting_down_;
        ProcessRole respawn_role = it->role;
        std::string respawn_name = it->name;

        const auto now = std::chrono::steady_clock::now();

        // A crash-looping child, once its backoff has latched at the ceiling, would
        // otherwise repeat its per-event lines below — this exit notice, the backoff
        // warning, the respawn notice — every few seconds for as long as it lasts:
        // thousands of lines a day on a shipboard server nobody restarts before port.
        // We never stop retrying (a process that gives up for good is worse at sea),
        // but past the ceiling we stop narrating each attempt and print one summary
        // line every few minutes instead. The ramp up to the ceiling still logs each
        // step. The storm is latched and read per child name, so one child's flood
        // never hides another's. Recovery is decided HERE, before the exit notice: the
        // death that ends a storm (a child up over a minute, then dead) clears the
        // latch first, so its own exit line — the one carrying new information — still
        // prints. Only while respawning: during shutdown these exits are expected and
        // stay quiet on their own.
        // The decision and the narration are split on purpose. The latch must be
        // cleared HERE, before `storm` is read below, or the exit line of the death
        // that ends a storm is swallowed (the latch would still read set). But the
        // recovery NOTICE is deferred to a local and printed after the exit line, so
        // the log reads forward: first the death, then the line saying it ended the
        // storm — not "recovered" ahead of the exit that triggered it.
        bool storm_ended = false;
        int  ended_attempts = 0;
        RespawnState* rsp = nullptr;
        if (respawn)
        {
            rsp = &respawn_state_[respawn_name];
            if (now - rsp->last_time > std::chrono::seconds(60))
            {
                // Stable for over a minute, then died. Clear the latch and remember
                // whether we were in a storm — the notice comes after the exit line.
                storm_ended    = rsp->storm;
                ended_attempts = rsp->storm_attempts;
                rsp->rapid_count    = 0;
                rsp->storm          = false;
                rsp->storm_attempts = 0;
                rsp->last_storm_log = {};
            }
        }
        const bool storm = rsp && rsp->storm;

        const std::string exit_desc = WIFEXITED(status)
            ? fmt::format("code {}", WEXITSTATUS(status))
            : WIFSIGNALED(status) ? fmt::format("signal {}", WTERMSIG(status))
                                  : std::string("unknown status");

        if (!storm)
        {
            if (WIFEXITED(status))
                logger_->notice("{} '{}' (pid={}) exited with {}",
                    role_name(it->role), it->name, pid, exit_desc);
            else if (WIFSIGNALED(status))
                logger_->warn("{} '{}' (pid={}) killed by {}",
                    role_name(it->role), it->name, pid, exit_desc);
        }

        // After the exit line: the quiet summaries stop here and silence should not be
        // the only sign the storm ended. "since its previous exit", not "was up":
        // last_time is stamped at reaping, so the interval is death-to-death and
        // includes the backoff itself.
        if (storm_ended)
            logger_->notice("{} '{}' recovered: over a minute since its previous exit "
                            "— storm ended after {} respawn attempts",
                            role_name(respawn_role), respawn_name, ended_attempts);

        children_.erase(it);

        if (respawn)
        {
            // ── Respawn rate limiting ────────────────────────────────────────
            // Prevent tight crash loops: if a child exits too quickly, delay the
            // respawn with exponential backoff (1s → 2s → 4s, max 30s). Recovery
            // (60s stable) is handled above, before the exit notice. All state is
            // this child's own.
            RespawnState& rs = *rsp;
            int delay = 0;
            if (now - rs.last_time < std::chrono::seconds(2))
            {
                ++rs.rapid_count;
                if (rs.rapid_count > 3)
                    // Cap the shift exponent: with no timer loop (master_loop_ null)
                    // respawns are immediate, rapid_count can run away, and 1<<n is UB
                    // past the width of int. 1<<5 = 32 already exceeds the 30s ceiling.
                    delay = std::min(1 << std::min(rs.rapid_count - 3, 5), 30);
            }
            rs.last_time = now;

            // Latch the storm the first time the backoff reaches its ceiling, and hold
            // the ceiling while latched. Without the hold the delay oscillates 30→0 —
            // after a delayed respawn the child no longer dies "fast", rapid_count stops
            // growing and delay falls back to 0 — so the master would fork twice per
            // ceiling window while the summary still claims "backoff at its 30s ceiling".
            // Latched, the ceiling is real and the fork rate in a storm halves. Recovery
            // above clears the latch.
            if (delay >= 30)
                rs.storm = true;
            else if (rs.storm)
                delay = 30;
            const bool storm_now = rs.storm;

            // The fork itself. It used to run right here, and the backoff above was a
            // ::sleep() right before it — but reap_children() runs inside the master's
            // SIGCHLD handler, so that sleep stalled the master's entire event loop:
            // for up to 30s it reaped no other dead children and answered no signals,
            // SIGTERM included, so a shutdown could hang half a minute. Now the fork is
            // packaged and, when throttled, scheduled on the master loop by a one-shot
            // timer; the master keeps turning through the delay. The rate limit itself
            // is unchanged — a crash loop is still spaced out, just not by freezing the
            // master to do it.
            auto do_respawn = [this, respawn_role, respawn_name, quiet = storm_now]() mutable {
                // Re-check at fire time, not only when this was scheduled: a delayed
                // respawn could otherwise fork a new child into a master that began
                // shutting down during the backoff. Shutdown wins that race by two
                // orders of magnitude and the SIGKILL escalation would reap the stray
                // anyway, but the guard removes the reasoning entirely.
                if (shutting_down_)
                    return;

                if (!quiet)
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
            };

            // Backoff logging. Up the ramp (delay 2→4→8→16s) each step is announced —
            // there are only a few, and they show the escalation. Once the storm is
            // latched (delay reached the 30s ceiling, and held there above) the
            // per-attempt warning is replaced by one summary line at most every few
            // minutes, carrying the running attempt tally and the last exit status so
            // the operator sees the process is failing and how — without a line every
            // few seconds. The retry never stops, only the narration does.
            if (storm_now)
            {
                ++rs.storm_attempts;
                if (rs.last_storm_log.time_since_epoch().count() == 0 ||
                    now - rs.last_storm_log >= kStormLogInterval)
                {
                    logger_->warn("{} '{}' keeps failing to stay up — {} respawn attempts "
                                  "so far, last exit {}, backoff at its 30s ceiling; still "
                                  "retrying, this line at most every {} min",
                                  role_name(respawn_role), respawn_name, rs.storm_attempts,
                                  exit_desc,
                                  std::chrono::duration_cast<std::chrono::minutes>(
                                      kStormLogInterval).count());
                    rs.last_storm_log = now;
                    // storm_attempts is the running tally since the storm latched; it is
                    // reset only on recovery, so the count only grows across summaries.
                }
            }
            else if (delay > 0)
            {
                logger_->warn("rapid respawn detected ({} in a row) — delaying {} '{}' by {}s",
                              rs.rapid_count, role_name(respawn_role), respawn_name, delay);
            }

            if (delay > 0 && master_loop_)
            {
                // One-shot (repeat=false): fire once after the backoff, then drop.
                master_loop_->add_timer(std::chrono::seconds(delay),
                    [do_respawn = std::move(do_respawn)]() mutable { do_respawn(); },
                    /*repeat=*/false);
            }
            else
            {
                do_respawn();
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

bool Application::rolling_restart()
{
    // The same reading as a start (read_config): the file, validate(), oauth2/
    // and sites/ into locals, swapped in one step only when all of it is good.
    // The new processes fork from this master, so what it holds is what they
    // get — the providers included, which this path used to leave as they were
    // at start.
    try
    {
        load_config(/*reload=*/true);
    }
    catch (const ConfigError& e)
    {
        logger_->error("config reload failed: {} — keeping old config", e.what());
        return false;
    }

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

    return true;
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

    // Only root can switch user/group — skip silently when running unprivileged
    if (::getuid() != 0)
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

        bool have_file = false;

        if (!settings_.postgres_log.empty()) {
            try {
                std::filesystem::create_directories(settings_.postgres_log.parent_path());
                pg_logger_->set_file_target(settings_.postgres_log.string(), settings_.log_max_size,
                                             settings_.log_keep_rotated, settings_.log_compress);
                have_file = true;
            } catch (const std::exception& e) {
                logger_->warn("cannot open postgres log '{}': {}",
                    settings_.postgres_log.string(), e.what());
            }
        }

        // Where the PostgreSQL traffic goes decides how loud it may be.
        //
        // A dedicated postgres.log is asked for on purpose and protected by file
        // permissions, so it keeps the full statement journal that makes "what
        // exactly went to the database" answerable. Without one the only outlet is
        // stderr — which on a container deployment means `docker logs`, readable by
        // anyone with the daemon socket — so it follows log.level like everything
        // else instead of always printing every statement at debug.
        if (have_file) {
            pg_logger_->set_level(LogLevel::debug);
        } else {
            pg_logger_->add_target(std::make_unique<StderrTarget>());
            pg_logger_->set_level(safe_log_level(settings_.log_level));
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

PgPool& Application::db_pool(std::string_view role)
{
    // "worker" or empty → default pool
    if (role.empty() || role == "worker")
        return db_pool();

    std::string key(role);

    // Return cached pool if already created
    auto it = named_pools_.find(key);
    if (it != named_pools_.end())
        return *it->second;

    // Determine conninfo for the requested role
    std::string conninfo;
    if (role == "helper")
        conninfo = settings_.pg_conninfo_helper;
    else if (role == "kernel")
        conninfo = settings_.pg_conninfo_kernel;
    else
        throw std::logic_error(fmt::format("unknown db_pool role: '{}'", role));

    if (conninfo.empty())
        throw std::logic_error(fmt::format("postgres.{} not configured", role));

    if (!worker_loop_)
        throw std::logic_error("db_pool(role) called before event loop is ready");

    // Create pool with the same logger and pool size settings
    auto pool = std::make_unique<PgPool>(*worker_loop_, std::move(conninfo),
        static_cast<std::size_t>(settings_.pg_pool_min),
        static_cast<std::size_t>(settings_.pg_pool_max),
        pg_logger_.get());
    pool->start();

    auto& ref = *pool;
    named_pools_.emplace(std::move(key), std::move(pool));
    return ref;
}

#endif // WITH_POSTGRESQL

// ─── Stream logger ───────────────────────────────────────────────────────────

Logger& Application::stream_logger()
{
    if (!stream_logger_) {
        stream_logger_ = std::make_unique<Logger>();
        stream_logger_->set_level(safe_log_level(settings_.log_level));
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

void Application::set_ws_upgrade_filter(WsUpgradeFilter f)
{
    ws_upgrade_filter_ = std::move(f);
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
        listener = std::make_shared<TcpListener>(port, settings_.server_backlog,
                                                 settings_.server_listen);
    }
    http_port_ = listener->local_port();

    logger_->notice("HTTP server listening on {}:{}", settings_.server_listen.empty() ? "*" : settings_.server_listen, http_port_);

    // Accept loop — under APOSTOL_EPOLL_ET the listener fires one event per
    // readable transition, so we must drain the accept queue each time.
    // accept_drain loops until EAGAIN; rearm_io re-enables further events.
    // Under LT (flag OFF) the loop is equivalent — single accept per event
    // still works because LT re-fires until the queue empties, but the
    // drain shape is harmless and keeps one code path.
    int listen_fd = listener->fd();
    loop.add_io(listen_fd, EPOLLIN,
        [this, &loop, listener, listen_fd](uint32_t)
        {
            // Under APOSTOL_EPOLL_ET the listener is disarmed by ONESHOT
            // after this event. Always rearm, even on exceptions from the
            // accept handlers — otherwise a single bad alloc or handler
            // throw would permanently stop accepting new connections.
            try {
                listener->accept_drain(
                    [this, &loop](TcpConnection raw_conn)
                    {
                        auto http_conn = std::make_shared<HttpConnection>(std::move(raw_conn), &loop);
                        int  conn_fd   = http_conn->fd();

                        loop.add_io(conn_fd, EPOLLIN | EPOLLRDHUP,
                            [this, &loop, http_conn, conn_fd](uint32_t events)
                            {
                                // Always re-arm or detach this fd before returning,
                                // even if a handler throws. Leaking it armed-but-
                                // disabled under ET = dark connection forever.
                                try {
                                    // Drain pending async writes (sendfile, buffered responses)
                                    if (events & EPOLLOUT)
                                        http_conn->on_writable();

                                    if (!(events & (EPOLLIN | EPOLLRDHUP))) {
                                        loop.rearm_io(conn_fd);
                                        return;
                                    }

                                    bool upgraded = false;

                                    bool keep = http_conn->on_readable(
                                        [this, &loop, &http_conn, conn_fd, &upgraded]
                                        (const HttpRequest& req, HttpResponse& resp)
                                        {
                                            req.connection_ctx = http_conn;

                                            if (ws_handler_ && is_ws_upgrade(req))
                                            {
                                                // Refusal BEFORE the handshake: resp goes out below
                                                // as ordinary HTTP. A filter that refused and left
                                                // the status at 2xx forgot to set it — an upgrade
                                                // request answered 200 without Upgrade is a client
                                                // stuck waiting, so make it a plain refusal.
                                                if (ws_upgrade_filter_ && !ws_upgrade_filter_(req, resp))
                                                {
                                                    if (resp.status_code() < 300)
                                                        resp.set_status(403, "Forbidden")
                                                            .set_body("WebSocket upgrade refused");
                                                    return;
                                                }

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

                                            if (!module_manager_.execute(req, resp))
                                                resp.set_status(404, "Not Found")
                                                    .set_body("404 Not Found");
                                        });

                                    if (upgraded)
                                        return;   // ws_handler re-registered its own I/O
                                    if (!keep)
                                        loop.remove_io(conn_fd);
                                    else
                                        loop.rearm_io(conn_fd);
                                } catch (const std::exception& e) {
                                    if (logger_)
                                        logger_->error("HTTP handler threw: {} — dropping connection fd={}",
                                                       e.what(), conn_fd);
                                    loop.remove_io(conn_fd);
                                } catch (...) {
                                    if (logger_)
                                        logger_->error("HTTP handler threw unknown exception — dropping connection fd={}",
                                                       conn_fd);
                                    loop.remove_io(conn_fd);
                                }
                            });
                    });
            } catch (const std::exception& e) {
                if (logger_)
                    logger_->error("Accept loop threw: {} — listener will rearm", e.what());
            } catch (...) {
                if (logger_)
                    logger_->error("Accept loop threw unknown exception — listener will rearm");
            }
            loop.rearm_io(listen_fd);
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
                for (auto& [_, pool] : named_pools_)
                    pool->heartbeat();
            });
    }
#endif
}

} // namespace apostol
