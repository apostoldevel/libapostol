// process_model_test — the process model around the master: what a failed
// fork, a pid file, a daemon's umask, a failed privilege switch and a crash
// report do. One case per run; this process drives, a forked child runs the
// application, and the verdict is read from its exit, its files and its log.
//
//   fork_retry    a master and one worker. The master's process limit is set
//                 below what it holds and its worker killed: the master stays
//                 up, logs the refused fork and starts the worker once the
//                 limit is back. The same for a reload (SIGHUP). Then SIGTERM:
//                 exit 0, the pid file gone.
//   own_pid       the pid file names the starting process itself (a container
//                 started again): the start goes on. Naming another live
//                 process it is still refused.
//   pid_mode      -d with umask 0: the pid file is 0644, not writable by others.
//   privilege     "process.user" that cannot be switched to (getuid() reads 0
//                 here): the process ends with code 1 and says why, it does
//                 not serve on.
//   crash_fd      the crash report while the log cannot be opened by its path
//                 any more: it reaches the file through the descriptor opened
//                 when the path was set.
//   crash_reload  "log.file" changed and SIGHUP: a crash after it reports into
//                 the new file.
//
// Usage: process_model_test <case> <dir>
// Exit: 0 pass, 1 fail, 77 skipped (run as root: limits and permissions do
// not hold for root), 2 usage.

#include "apostol/application.hpp"
#include "apostol/crash_handler.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace apostol;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// The privilege switch runs only for root. The library's getuid() comes from
// here: 0 while g_fake_root, the real one otherwise.
static bool g_fake_root = false;
extern "C" uid_t getuid() noexcept
{
    return g_fake_root ? 0 : static_cast<uid_t>(::syscall(SYS_getuid));
}

namespace
{

int  g_failed = 0;
fs::path g_dir;

void check(bool ok, const std::string& what)
{
    std::printf("process_model_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::trunc);
    f << s;
}

std::size_t count_of(const std::string& text, const std::string& what)
{
    std::size_t n = 0;
    for (auto pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + 1))
        ++n;
    return n;
}

fs::path error_log() { return g_dir / "logs/error.log"; }
fs::path pid_file()  { return g_dir / "logs/apostol-test.pid"; }
fs::path conf_file() { return g_dir / "test.json"; }

bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds limit)
{
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (cond())
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return cond();
}

bool log_has(const std::string& what, std::size_t times = 1)
{
    return count_of(read_file(error_log()), what) >= times;
}

class TestApp final : public Application
{
public:
    TestApp() : Application("process_model_test") {}
};

// The application in a child of this process, with -p and -c of the case and
// `extra` arguments; `before` runs in the child first.
pid_t start_app(std::vector<std::string> extra = {}, std::function<void()> before = {})
{
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid != 0)
        return pid;

    // No core files from the crash cases.
    struct rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    if (before)
        before();

    std::vector<std::string> args{"process_model_test", "-p", g_dir.string() + "/",
                                  "-c", conf_file().string()};
    args.insert(args.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    int rc = 0;
    {
        TestApp app;
        rc = app.run(static_cast<int>(args.size()), argv.data());
    }
    std::fflush(stdout);
    ::_exit(rc);
}

// Exit status of `pid` within `limit`; -1 when still running (it is killed).
int wait_exit(pid_t pid, std::chrono::milliseconds limit, int* signal_out = nullptr)
{
    int status = 0;
    const bool done = wait_for([&] { return ::waitpid(pid, &status, WNOHANG) == pid; }, limit);
    if (!done) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return -1;
    }
    if (signal_out)
        *signal_out = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

bool alive(pid_t pid)
{
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == 0;
}

// A port nobody holds right now: the master binds a listener, validate()
// takes no 0. Nothing here serves on it.
int free_port()
{
    int port = 0;
    if (int fd = ::socket(AF_INET, SOCK_STREAM, 0); fd >= 0) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len     = sizeof(a);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0
            && ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
            port = ntohs(a.sin_port);
        ::close(fd);
    }
    return port;
}

void config(const std::string& process, const std::string& log_file = "logs/error.log")
{
    static const int port = free_port();
    write_file(conf_file(), fmt::format(R"({{
  "workers": 1,
  "process": {{ {} }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice", "file": "{}" }}
}}
)", process, port, log_file));
}

// ─── fork_retry ──────────────────────────────────────────────────────────────

bool set_nproc(pid_t pid, rlim_t soft)
{
    struct rlimit old{};
    if (::prlimit(pid, RLIMIT_NPROC, nullptr, &old) != 0)
        return false;
    struct rlimit rl{soft, old.rlim_max};
    return ::prlimit(pid, RLIMIT_NPROC, &rl, nullptr) == 0;
}

void case_fork_retry()
{
    config(R"("master": true, "helper": false, "user": "")");
    const pid_t master = start_app();
    const std::string spawned = "spawned worker 'worker#1'";
    check(wait_for([&] { return log_has(spawned); }, 10s), "the worker is spawned");

    struct rlimit nproc{};
    ::prlimit(master, RLIMIT_NPROC, nullptr, &nproc);

    // The worker's pid from the line "spawned worker 'worker#1' pid=N".
    const auto worker_pid = [&](std::size_t nth) -> pid_t {
        const auto text = read_file(error_log());
        std::size_t pos = std::string::npos;
        for (std::size_t i = 0, from = 0; i < nth; ++i, from = pos + 1)
            if ((pos = text.find(spawned, from)) == std::string::npos)
                return 0;
        const auto at = text.find("pid=", pos);
        return at == std::string::npos ? 0 : static_cast<pid_t>(std::atoi(text.c_str() + at + 4));
    };

    // 1. A respawn the kernel refuses.
    const pid_t worker = worker_pid(1);
    check(worker > 1 && worker != master, fmt::format("the worker's pid is read from the log ({})", worker));
    if (worker <= 1 || worker == master) {
        ::kill(master, SIGKILL);
        ::waitpid(master, nullptr, 0);
        return;
    }
    check(set_nproc(master, 1), "the master's process limit lowered");
    ::kill(worker, SIGKILL);
    check(wait_for([&] { return log_has("cannot start worker 'worker#1'"); }, 5s),
          "the refused fork is logged");
    std::this_thread::sleep_for(500ms);
    check(alive(master), "the master stays up after a refused respawn");
    set_nproc(master, nproc.rlim_cur);
    check(wait_for([&] { return log_has(spawned, 2); }, 15s),
          "the worker is started again once the limit is back");

    // 2. A reload the kernel refuses.
    check(set_nproc(master, 1), "the master's process limit lowered again");
    ::kill(master, SIGHUP);
    check(wait_for([&] { return log_has("cannot start worker 'worker#1'", 2); }, 5s),
          "the refused fork of the reload is logged");
    std::this_thread::sleep_for(500ms);
    check(alive(master), "the master stays up after a refused reload");
    set_nproc(master, nproc.rlim_cur);
    check(wait_for([&] { return log_has(spawned, 3); }, 15s),
          "the new worker is started once the limit is back");

    ::kill(master, SIGTERM);
    check(wait_exit(master, 15s) == 0, "SIGTERM: the master exits 0");
    check(!fs::exists(pid_file()), "the pid file is removed");
}

// ─── own_pid ─────────────────────────────────────────────────────────────────

void case_own_pid()
{
    config(R"("master": false, "helper": false, "user": "")");

    // The pid file names the process that starts.
    const pid_t own = start_app({}, [] { write_file(pid_file(), fmt::format("{}\n", ::getpid())); });
    check(wait_for([&] { return log_has("single process started"); }, 10s),
          "a pid file naming the process itself does not stop its start");
    ::kill(own, SIGTERM);
    check(wait_exit(own, 10s) == 0, "it runs and exits 0 on SIGTERM");

    // The pid file names another live process (this one): refused, as ever.
    fs::remove(error_log());
    write_file(pid_file(), fmt::format("{}\n", ::getpid()));
    const pid_t other = start_app();
    check(wait_exit(other, 10s) == 1, "a pid file naming another live process: the start is refused");
    check(log_has("is already running"), "and says so");
    fs::remove(pid_file());
}

// ─── pid_mode ────────────────────────────────────────────────────────────────

void case_pid_mode()
{
    config(R"("master": false, "helper": false, "user": "")");
    const pid_t launcher = start_app({"-d"}, [] { ::umask(0); });
    check(wait_exit(launcher, 10s) == 0, "-d: the launcher exits 0");
    check(wait_for([&] { return log_has("single process started"); }, 10s), "the daemon runs");

    struct stat st{};
    const bool have = ::stat(pid_file().c_str(), &st) == 0;
    check(have, "the daemon writes its pid file");
    const unsigned mode = have ? (st.st_mode & 07777) : 0;
    check(mode == 0644, fmt::format("the pid file is 0644 under umask 0 (it is {:o})", mode));

    const pid_t daemon = static_cast<pid_t>(std::atoi(read_file(pid_file()).c_str()));
    if (daemon > 1)
        ::kill(daemon, SIGTERM);
    check(wait_for([&] { return !fs::exists(pid_file()); }, 10s), "the daemon stops on SIGTERM");
}

// ─── privilege ───────────────────────────────────────────────────────────────

void case_privilege()
{
    // A user that does not exist.
    config(R"("master": false, "helper": false, "user": "no-such-user-apostol-test", "group": "")");
    pid_t pid = start_app({}, [] { g_fake_root = true; });
    check(wait_exit(pid, 10s) == 1, "an unknown process.user: the process ends with code 1");
    check(log_has("cannot drop privileges: user 'no-such-user-apostol-test' not found"),
          "and logs why");

    // A user that exists, and a switch the kernel refuses (not root in fact).
    const struct passwd* pw = ::getpwnam("nobody");
    if (!pw) {
        std::printf("process_model_test: no user 'nobody' — the refused switch not run\n");
        return;
    }
    fs::remove(error_log());
    config(R"("master": false, "helper": false, "user": "nobody", "group": "")");
    pid = start_app({}, [] { g_fake_root = true; });
    check(wait_exit(pid, 10s) == 1, "a refused setgid(): the process ends with code 1");
    check(log_has("cannot drop privileges: setgid("), "and logs why");
}

// ─── crash_fd ────────────────────────────────────────────────────────────────

void case_crash_fd()
{
    const fs::path log = g_dir / "logs/crash.log";
    write_file(log, "");
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        struct rlimit no_core{0, 0};
        ::setrlimit(RLIMIT_CORE, &no_core);
        install_crash_handler(log.string());
        // From here the path is refused to this process, as to a child that
        // runs as another user than the one that made the log.
        ::chmod(log.c_str(), 0444);
        ::raise(SIGSEGV);
        ::_exit(0);
    }
    int sig = 0;
    wait_exit(pid, 30s, &sig);
    ::chmod(log.c_str(), 0644);
    check(sig == SIGSEGV, "the child ends by its signal");
    check(read_file(log).find("-----END CRASH REPORT-----") != std::string::npos,
          "the report reaches the log it may not open any more");
}

// ─── crash_reload ────────────────────────────────────────────────────────────

void case_crash_reload()
{
    config(R"("master": false, "helper": false, "user": "")", "logs/a.log");
    const pid_t pid = start_app();
    const fs::path a = g_dir / "logs/a.log", b = g_dir / "logs/b.log";
    check(wait_for([&] { return read_file(a).find("single process started") != std::string::npos; }, 10s),
          "the process runs, logging into a.log");

    config(R"("master": false, "helper": false, "user": "")", "logs/b.log");
    ::kill(pid, SIGHUP);
    check(wait_for([&] { return fs::exists(b); }, 10s), "SIGHUP: the log moves to b.log");
    std::this_thread::sleep_for(200ms);

    ::kill(pid, SIGSEGV);
    int sig = 0;
    wait_exit(pid, 30s, &sig);
    check(sig == SIGSEGV, "the process ends by its signal");
    check(read_file(b).find("-----END CRASH REPORT-----") != std::string::npos,
          "the crash report is in the new log");
    check(read_file(a).find("CRASH REPORT") == std::string::npos, "and not in the old one");
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: process_model_test <case> <dir>\n");
        return 2;
    }
    const std::string name = argv[1];
    g_dir = fs::absolute(argv[2]);

    if (::geteuid() == 0) {
        std::printf("process_model_test: run as root — limits and permissions do not hold, skipped\n");
        return 77;
    }

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "logs");

    if      (name == "fork_retry")   case_fork_retry();
    else if (name == "own_pid")      case_own_pid();
    else if (name == "pid_mode")     case_pid_mode();
    else if (name == "privilege")    case_privilege();
    else if (name == "crash_fd")     case_crash_fd();
    else if (name == "crash_reload") case_crash_reload();
    else {
        std::fprintf(stderr, "process_model_test: unknown case '%s'\n", name.c_str());
        return 2;
    }

    std::printf("process_model_test: %s — %s (log %s)\n", name.c_str(),
                g_failed ? "FAIL" : "PASS", error_log().c_str());
    return g_failed ? 1 : 0;
}
