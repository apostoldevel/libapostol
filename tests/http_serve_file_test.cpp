// http_serve_file_test — ApostolModule::try_files() answers with the file as
// the body (HttpResponse::set_file()), and HttpConnection sends it from the file.
//
//   1. memory: try_files() on a 64 MB file does not grow the process by the
//      file — it is opened, not read;
//   2. the wire: send_response() gives the client the status line, the
//      headers the caller added after try_files(), Content-Length of the file
//      and the file byte for byte;
//   3. HEAD: Content-Length of the file and no body.
//
// Usage: http_serve_file_test

#include "apostol/apostol_module.hpp"
#include "apostol/http.hpp"
#include "apostol/tcp.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace apostol;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::size_t rss_bytes()
{
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

class Probe final : public ApostolModule
{
public:
    std::string_view name() const override { return "Probe"; }
    bool enabled() const override { return true; }
    bool serve(const fs::path& root, const std::string& path, HttpResponse& resp, bool head_only)
    {
        HttpRequest req;
        req.method = head_only ? "HEAD" : "GET";
        req.path = path;
        req.version = "HTTP/1.1";
        return try_files(root, req, resp, head_only, {});
    }

protected:
    void init_methods() override {}
};

// The byte at offset i of the test file.
char byte_at(std::size_t i) { return static_cast<char>('a' + (i * 7 + i / 4096) % 26); }

// Sends `resp` through an HttpConnection; returns everything the client read.
std::string send_and_read(const HttpResponse& resp, std::size_t expect)
{
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { std::perror("socketpair"); std::exit(1); }
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    sockaddr_storage ss{};
    ss.ss_family = AF_UNIX;
    std::string got;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        char buf[65536];
        while (got.size() < expect) {
            ssize_t n = ::read(sv[1], buf, sizeof buf);
            if (n <= 0) break;
            got.append(buf, static_cast<std::size_t>(n));
        }
        done = true;
    });
    {
        HttpConnection conn(TcpConnection(sv[0], ss, sizeof(sa_family_t)), nullptr);
        conn.send_response(resp);
        for (int i = 0; i < 2000000 && !done && !conn.closed(); ++i)
            if (!conn.on_writable())
                break;
    }
    ::shutdown(sv[1], SHUT_RDWR);
    reader.join();
    ::close(sv[1]);
    return got;
}

} // namespace

int main()
{
    const std::size_t size = 64u * 1024 * 1024;
    const fs::path root = fs::temp_directory_path() / ("http_serve_file_test." + std::to_string(::getpid()));
    fs::create_directories(root);
    {
        std::string data(size, '\0');
        for (std::size_t i = 0; i < size; ++i)
            data[i] = byte_at(i);
        FILE* f = std::fopen((root / "big.bin").c_str(), "wb");
        if (!f || std::fwrite(data.data(), 1, size, f) != size) { std::perror("write"); return 1; }
        std::fclose(f);
    }

    Probe m;

    // 1. memory
    {
        HttpResponse resp;
        const auto before = rss_bytes();
        const bool sent = m.serve(root, "/big.bin", resp, false);
        const auto grown = rss_bytes() - before;
        check(sent && resp.status_code() == 200, "GET: not served");
        check(grown < size / 4, "GET: try_files grew the process by " + std::to_string(grown >> 20) +
                                " MB for a " + std::to_string(size >> 20) + " MB file");
    }

    // 2. the wire
    {
        HttpResponse resp;
        m.serve(root, "/big.bin", resp, false);
        resp.set_header("Cache-Control", "max-age=60");   // added after try_files
        const auto head = resp.serialize_head();
        const auto wire = send_and_read(resp, head.size() + size);
        check(wire.compare(0, head.size(), head) == 0, "GET: header block differs:\n" + wire.substr(0, 300));
        check(head.find("Content-Length: " + std::to_string(size) + "\r\n") != std::string::npos,
              "GET: Content-Length is not the file's size:\n" + head);
        check(head.find("Cache-Control: max-age=60\r\n") != std::string::npos, "GET: caller's header lost:\n" + head);
        check(wire.size() == head.size() + size, "GET: " + std::to_string(wire.size() - head.size()) +
                                                 " body bytes, expected " + std::to_string(size));
        bool same = wire.size() == head.size() + size;
        for (std::size_t i = 0; same && i < size; ++i)
            same = wire[head.size() + i] == byte_at(i);
        check(same, "GET: body differs from the file");
    }

    // 3. HEAD
    {
        HttpResponse resp;
        m.serve(root, "/big.bin", resp, true);
        const auto head = resp.serialize_head();
        const auto wire = send_and_read(resp, head.size() + 1);
        check(head.find("Content-Length: " + std::to_string(size) + "\r\n") != std::string::npos,
              "HEAD: Content-Length is not the file's size:\n" + head);
        check(wire == head, "HEAD: " + std::to_string(wire.size() - head.size()) + " body bytes sent");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("http_serve_file: all checks passed\n");
    return 0;
}
