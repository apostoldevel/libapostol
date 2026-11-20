// file_logger_test — a logger of a module's own, in every role of a node.
//
// A master with one worker, the helper and a custom process "probe". Each of
// them writes a line every 50 ms into stream_logger() and into
// file_logger("probe", resolve_path("logs/named/probe.log")).
//
//   1. stream_logger() writes to the file "log.stream" names, not to the
//      compiled default.
//   2. file_logger() writes to the file it was given; its directory is made.
//   3. Both files moved away and SIGUSR1 to the master: every role writes
//      both files again at their paths.
//
// "relative": the same with the prefix given relative to the working
// directory — each path is taken from the prefix once, not twice.
//
// Usage: file_logger_test <dir> [relative]
// Exit: 0 pass, 1 fail, 2 usage.

#include "apostol/application.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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

int      g_failed   = 0;
fs::path g_dir;
bool     g_relative = false;

void check(bool ok, const std::string& what)
{
    std::printf("file_logger_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
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

fs::path conf_file()    { return g_dir / "test.json"; }
fs::path stream_file()  { return g_dir / "logs/streams/configured.log"; }
fs::path default_file() { return g_dir / "logs/stream.log"; }
fs::path named_file()   { return g_dir / "logs/named/probe.log"; }

const std::vector<std::string> kRoles{"worker", "helper", "probe"};

// Every role has written a line into `p`.
bool all_roles_in(const fs::path& p)
{
    const auto text = read_file(p);
    for (const auto& role : kRoles)
        if (text.find("tick " + role) == std::string::npos)
            return false;
    return true;
}

std::string roles_missing(const fs::path& p)
{
    const auto text = read_file(p);
    std::string out;
    for (const auto& role : kRoles)
        if (text.find("tick " + role) == std::string::npos)
            out += (out.empty() ? "" : ", ") + role;
    return out.empty() ? "none" : out;
}

// A line every 50 ms into both loggers, on the loop of the role.
void tick(Application& app, EventLoop& loop, const std::string& role)
{
    loop.add_timer(50ms, [&app, role] {
        app.stream_logger().notice("tick {}", role);
        app.file_logger("probe", app.resolve_path("logs/named/probe.log")).notice("tick {}", role);
    });
}

class Probe final : public ProcessModule
{
public:
    std::string_view name() const override { return "probe"; }
    void on_start(EventLoop& loop, Application& app) override { tick(app, loop, "probe"); }
};

class TestApp final : public Application
{
public:
    TestApp() : Application("file_logger_test") {}

protected:
    void on_worker_start(EventLoop& loop) override { tick(*this, loop, "worker"); }
    void on_helper_start(EventLoop& loop) override { tick(*this, loop, "helper"); }

    void create_custom_processes() override
    {
#ifdef WITH_POSTGRESQL
        add_custom_process(std::unique_ptr<ProcessModule>(std::make_unique<Probe>()));
#else
        add_custom_process("probe", [this](EventLoop& loop) { tick(*this, loop, "probe"); });
#endif
    }
};

pid_t start_app()
{
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid != 0)
        return pid;

    std::string prefix = g_dir.string() + "/";
    if (g_relative && ::chdir(g_dir.parent_path().c_str()) == 0)
        prefix = g_dir.filename().string() + "/";
    std::vector<std::string> args{"file_logger_test", "-p", prefix, "-c", conf_file().string()};
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

int wait_exit(pid_t pid, std::chrono::milliseconds limit)
{
    int status = 0;
    const bool done = wait_for([&] { return ::waitpid(pid, &status, WNOHANG) == pid; }, limit);
    if (!done) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

// A port nobody holds right now: the master binds a listener.
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

// Waits until every role has written into `p`; the check names who has not.
void expect_all_roles(const fs::path& p, std::chrono::milliseconds limit, const std::string& what)
{
    const bool ok = wait_for([&] { return all_roles_in(p); }, limit);
    check(ok, what + (ok ? "" : " (missing: " + roles_missing(p) + ")"));
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "relative")) {
        std::fprintf(stderr, "usage: file_logger_test <dir> [relative]\n");
        return 2;
    }
    g_relative = argc == 3;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    g_dir = fs::absolute(argv[1]);
    fs::remove_all(g_dir);
    fs::create_directories(g_dir / "logs");

    write_file(conf_file(), fmt::format(R"({{
  "workers": 1,
  "process": {{ "master": true, "helper": true, "stop_timeout": 5 }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice", "file": "logs/error.log", "stream": "logs/streams/configured.log" }}
}}
)", free_port()));

    const pid_t pid = start_app();

    expect_all_roles(stream_file(), 10s, "stream_logger() writes to log.stream in every role");
    check(read_file(default_file()).find("tick ") == std::string::npos,
          "stream_logger() writes nothing to the compiled default");
    expect_all_roles(named_file(), 10s, "file_logger() writes to its file in every role");

    std::error_code ec;
    fs::rename(stream_file(), stream_file().string() + ".old", ec);
    fs::rename(named_file(), named_file().string() + ".old", ec);
    ::kill(pid, SIGUSR1);

    expect_all_roles(stream_file(), 5s, "moved away, SIGUSR1: every role writes the stream log at its path again");
    expect_all_roles(named_file(), 5s, "moved away, SIGUSR1: every role writes a file_logger() at its path again");

    ::kill(pid, SIGTERM);
    check(wait_exit(pid, 15s) == 0, "SIGTERM: exit 0");

    std::printf("file_logger_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
