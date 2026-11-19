// ipc_frames_test — what the channels of the node do with datagrams no
// sender of this library writes, and with a request made when no descriptor
// is left. One case per run: a master and two workers; the first worker to
// start runs the case on its loop and writes the verdict, this process
// drives the master and reads it.
//
//   empty_datagram  zero-length datagrams written into every SEQPACKET
//                   socket the worker holds: the other worker's inbox stays
//                   open — a request to it is answered, the empty datagrams
//                   are counted as dropped.
//   part_flood      20000 first chunks that are not full chunks (total 0, or
//                   a short one): each is dropped at once, none is kept to be
//                   put together; the other worker answers in time.
//   no_timer        request() and gather() with every descriptor taken: no
//                   exception, each callback runs once, later, with "failed".
//
// Usage: ipc_frames_test <case> <dir>
// Exit: 0 pass, 1 fail, 2 usage.

#include "apostol/application.hpp"
#include "apostol/ipc.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
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

fs::path    g_dir;
std::string g_case;

fs::path error_log()    { return g_dir / "logs/error.log"; }
fs::path conf_file()    { return g_dir / "test.json"; }
fs::path verdict_file() { return g_dir / "verdict"; }
fs::path claim_file()   { return g_dir / "claim"; }

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

void verdict(bool ok, const std::string& what)
{
    std::ofstream f(verdict_file(), std::ios::app);
    f << (ok ? "ok   " : "FAIL ") << what << '\n';
}

void verdict_end()
{
    std::ofstream f(verdict_file(), std::ios::app);
    f << "end\n";
}

long long mono_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// The frame header as the library writes it (src/core/ipc.cpp).
struct FrameHeader
{
    std::uint32_t magic;
    std::uint8_t  kind;
    std::uint8_t  status;
    std::uint8_t  flags;
    std::uint8_t  type_len;
    std::uint16_t from_slot;
    std::uint16_t reserved;
    std::uint32_t from_generation;
    std::uint32_t id;
    std::uint32_t total;
    std::uint32_t offset;
    std::uint32_t timeout_ms;
};
static_assert(sizeof(FrameHeader) == 32);

std::vector<int> seqpacket_fds()
{
    std::vector<int> fds;
    for (int fd = 3; fd < 1024; ++fd)
    {
        int type = 0;
        socklen_t len = sizeof(type);
        if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_SEQPACKET)
            fds.push_back(fd);
    }
    return fds;
}

// One datagram, waiting for room up to 200 ms. False — the socket takes no more
// (nobody reads its other end).
bool put(int fd, const void* data, std::size_t n)
{
    for (;;)
    {
        if (::send(fd, data, n, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0)
            return true;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, 200) <= 0)
            return false;
    }
}

class TestApp final : public Application
{
public:
    TestApp() : Application("ipc_frames_test") { enable_ipc(); }

protected:
    void on_worker_start(EventLoop& loop) override
    {
        ipc().on("echo", [](const IpcPeer&, std::string_view b, IpcReply r) { r.send(std::string(b)); });
        ipc().on("dropped", [this](const IpcPeer&, std::string_view, IpcReply r) {
            r.send(std::to_string(ipc().stats().dropped));
        });
        const int claim = ::open(claim_file().c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (claim < 0)
            return;
        ::close(claim);
        loop_ = &loop;
        wait_for_other(mono_ms() + 10000);
    }

private:
    EventLoop* loop_{nullptr};
    int        other_{-1};

    void after(std::chrono::milliseconds ms, std::function<void()> fn)
    {
        loop_->add_timer(ms, std::move(fn), /*repeat=*/false);
    }

    void wait_for_other(long long until)
    {
        for (const auto& p : ipc().peers(ipc_workers))
            if (p.slot != ipc().self().slot)
                other_ = p.slot;
        if (other_ >= 0)
            return after(300ms, [this] { run_case(); });
        if (mono_ms() > until)
        {
            verdict(false, "the other worker did not come up");
            return verdict_end();
        }
        after(100ms, [this, until] { wait_for_other(until); });
    }

    void run_case()
    {
        if (g_case == "empty_datagram")
            empty_datagram();
        else if (g_case == "part_flood")
            part_flood();
        else if (g_case == "no_timer")
            no_timer();
        else
        {
            verdict(false, "unknown case " + g_case);
            verdict_end();
        }
    }

    void empty_datagram()
    {
        int sent = 0;
        for (int fd : seqpacket_fds())
            if (::send(fd, "", 0, MSG_DONTWAIT | MSG_NOSIGNAL) == 0)
                ++sent;
        verdict(sent > 0, fmt::format("zero-length datagrams written into {} sockets", sent));
        after(300ms, [this] {
            ipc().request(other_, "echo", "still here", [this](IpcResult r) {
                verdict(r.ok() && r.body == "still here",
                        fmt::format("the other worker answers after them ({})", ipc_error_name(r.error)));
                ipc().request(other_, "dropped", {}, [](IpcResult d) {
                    verdict(d.ok() && std::atoll(d.body.c_str()) >= 1,
                            fmt::format("and counts them as dropped ({})", d.ok() ? d.body : "no answer"));
                    verdict_end();
                }, 3s);
            }, 3s);
        });
    }

    void part_flood()
    {
        static constexpr int kFrames = 20000;
        const auto me = ipc().self();
        auto fds = seqpacket_fds();
        std::vector<char> frame(sizeof(FrameHeader) + 16, 'x');
        int sent = 0;
        for (int i = 1; i <= kFrames; ++i)
        {
            FrameHeader h{};
            h.magic           = 0x31435041;
            h.kind            = 1;   // one-way
            h.flags           = 1;   // first, not last
            h.from_slot       = static_cast<std::uint16_t>(me.slot);
            h.from_generation = me.generation;
            h.id              = static_cast<std::uint32_t>(i);
            // total 0 with nothing; or a chunk shorter than a full one
            h.total           = i % 2 ? 0 : static_cast<std::uint32_t>(Ipc::kChunk + 16);
            std::memcpy(frame.data(), &h, sizeof(h));
            const std::size_t n = i % 2 ? sizeof(h) : frame.size();
            for (auto& fd : fds)
                if (fd >= 0)
                {
                    if (put(fd, frame.data(), n))
                        ++sent;
                    else
                        fd = -1;
                }
        }
        verdict(sent >= kFrames, fmt::format("{} first chunks written ({} sockets)", sent, fds.size()));
        const long long t0 = mono_ms();
        ipc().request(other_, "dropped", {}, [t0](IpcResult r) {
            const long long ms = mono_ms() - t0;
            verdict(r.ok() && std::atoll(r.body.c_str()) >= kFrames,
                    fmt::format("the other worker dropped them ({}, need {})", r.ok() ? r.body : "no answer",
                                kFrames));
            verdict(r.ok() && ms < 2000, fmt::format("and answered in {} ms", ms));
            verdict_end();
        }, 10s);
    }

    void no_timer()
    {
        struct rlimit old{};
        ::getrlimit(RLIMIT_NOFILE, &old);
        struct rlimit low = old;
        low.rlim_cur = 256;
        ::setrlimit(RLIMIT_NOFILE, &low);
        std::vector<int> taken;
        for (int fd; (fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0;)
            taken.push_back(fd);
        const bool full = errno == EMFILE;

        auto gathered = std::make_shared<int>(0);
        auto missing  = std::make_shared<std::string>();
        auto asked    = std::make_shared<int>(0);
        auto error    = std::make_shared<std::string>();
        std::string threw;
        try
        {
            ipc().gather(ipc_workers, "echo", "x", [gathered, missing](IpcGathered g) {
                ++*gathered;
                for (const auto& [peer, e] : g.missing)
                    *missing += std::string(ipc_error_name(e)) + " ";
                if (!g.replies.empty())
                    *missing += "replies ";
            }, 1s);
            ipc().request(other_, "echo", "x", [asked, error](IpcResult r) {
                ++*asked;
                *error = ipc_error_name(r.error);
            }, 1s);
        }
        catch (const std::exception& e)
        {
            threw = e.what();
        }
        const int at_once = *gathered + *asked;
        for (int fd : taken)
            ::close(fd);
        ::setrlimit(RLIMIT_NOFILE, &old);

        verdict(full, fmt::format("every descriptor taken ({} opened)", taken.size()));
        verdict(threw.empty(), fmt::format("request() and gather() do not throw ({})", threw));
        verdict(at_once == 0, "no callback runs inside the call");
        after(2500ms, [gathered, missing, asked, error] {
            verdict(*gathered == 1, fmt::format("gather: done once ({})", *gathered));
            verdict(*missing == "failed failed ", fmt::format("both workers missing as failed ({})", *missing));
            verdict(*asked == 1 && *error == "failed",
                    fmt::format("request: done once with failed ({}, {})", *asked, *error));
            verdict_end();
        });
    }
};

int free_port()
{
    int port = 0;
    if (int fd = ::socket(AF_INET, SOCK_STREAM, 0); fd >= 0)
    {
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

bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds limit)
{
    const auto end = Clock::now() + limit;
    while (Clock::now() < end)
    {
        if (cond())
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return cond();
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ipc_frames_test <case> <dir>\n");
        return 2;
    }
    g_case = argv[1];
    g_dir  = fs::absolute(argv[2]);

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "logs");
    write_file(conf_file(), fmt::format(R"({{
  "workers": 2,
  "process": {{ "master": true, "helper": false, "user": "" }},
  "server": {{ "listen": "127.0.0.1", "port": {} }},
  "log": {{ "level": "notice", "file": "logs/error.log" }}
}}
)", free_port()));

    std::fflush(stdout);
    const pid_t master = ::fork();
    if (master == 0)
    {
        std::vector<std::string> args{"ipc_frames_test", "-p", g_dir.string() + "/", "-c", conf_file().string()};
        std::vector<char*> av;
        for (auto& a : args)
            av.push_back(a.data());
        av.push_back(nullptr);
        int rc = 0;
        {
            TestApp app;
            rc = app.run(static_cast<int>(args.size()), av.data());
        }
        std::fflush(stdout);
        ::_exit(rc);
    }

    const bool ended = wait_for([] { return read_file(verdict_file()).find("end\n") != std::string::npos; }, 40s);
    ::kill(master, SIGTERM);
    int status = 0;
    if (!wait_for([&] { return ::waitpid(master, &status, WNOHANG) == master; }, 15s))
    {
        ::kill(master, SIGKILL);
        ::waitpid(master, &status, 0);
    }

    const auto text = read_file(verdict_file());
    std::printf("%s", text.c_str());
    bool ok = ended && text.find("FAIL") == std::string::npos;
    if (!ended)
        std::printf("FAIL no verdict — the case did not finish\n");
    if (g_case == "empty_datagram" && read_file(error_log()).find("is closed") != std::string::npos)
    {
        std::printf("FAIL a channel was closed (see the log)\n");
        ok = false;
    }
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s the master exits 0\n", clean ? "ok  " : "FAIL");
    ok = ok && clean;
    std::printf("ipc_frames_test: %s — %s (log %s)\n", g_case.c_str(), ok ? "PASS" : "FAIL",
                error_log().c_str());
    return ok ? 0 : 1;
}
