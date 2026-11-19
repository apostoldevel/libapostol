// http_send_file_test — HttpConnection::send_file() over a socket pair.
//
//   1. the content type is written up to its first control byte, as
//      set_header() writes a value: a type carrying CR LF adds no header line;
//   2. a file that ends before the size announced in Content-Length (cut while
//      it is being sent) closes the connection after what was sent — the
//      client sees the answer cut — instead of leaving it open for the next
//      answer to be read as the rest of the body.
//
// Usage: http_send_file_test

#include "apostol/http.hpp"
#include "apostol/tcp.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace apostol;

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

struct Pair
{
    std::unique_ptr<HttpConnection> conn;
    int peer{-1};
    ~Pair() { if (peer >= 0) ::close(peer); }
};

void make_pair_conn(Pair& p)
{
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        std::exit(1);
    }
    int small = 4096;
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    sockaddr_storage ss{};
    ss.ss_family = AF_UNIX;
    p.conn = std::make_unique<HttpConnection>(TcpConnection(sv[0], ss, sizeof(sa_family_t)), nullptr);
    p.peer = sv[1];
}

// Everything readable now; eof set when the other side has shut down.
std::string drain(int fd, bool& eof)
{
    std::string out;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); continue; }
        if (n == 0) eof = true;
        break;
    }
    return out;
}

std::string temp_file(std::size_t size)
{
    char tmpl[] = "/tmp/http_send_file_test.XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { std::perror("mkstemp"); std::exit(1); }
    std::string chunk(65536, 'x');
    std::size_t left = size;
    while (left > 0) {
        std::size_t k = left < chunk.size() ? left : chunk.size();
        if (::write(fd, chunk.data(), k) != static_cast<ssize_t>(k)) { std::perror("write"); std::exit(1); }
        left -= k;
    }
    ::close(fd);
    return tmpl;
}

void content_type_case()
{
    auto file = temp_file(10);
    Pair p;
    make_pair_conn(p);
    p.conn->send_file(file, "text/plain\r\nX-Injected: 1");
    bool eof = false;
    std::string got = drain(p.peer, eof);
    const auto head = got.substr(0, got.find("\r\n\r\n"));
    check(head.find("X-Injected") == std::string::npos, "content type: injected header line present:\n" + head);
    check(head.find("Content-Type: text/plain\r\n") != std::string::npos, "content type: not text/plain:\n" + head);
    check(got.size() >= 10 && got.compare(got.size() - 10, 10, std::string(10, 'x')) == 0, "content type: body not sent");
    ::unlink(file.c_str());
}

void cut_file_case()
{
    const std::size_t size = 4 * 1024 * 1024;
    auto file = temp_file(size);
    Pair p;
    make_pair_conn(p);
    p.conn->send_file(file, "application/octet-stream");
    check(!p.conn->closed(), "cut file: closed before the cut");

    // The socket is full with most of the file still to go: cut it now.
    if (::truncate(file.c_str(), 0) != 0) { std::perror("truncate"); std::exit(1); }

    bool eof = false;
    std::string got;
    for (int i = 0; i < 10000 && !p.conn->closed(); ++i) {
        got += drain(p.peer, eof);
        if (!p.conn->on_writable())
            break;   // nothing pending any more
    }
    got += drain(p.peer, eof);

    const auto body_at = got.find("\r\n\r\n");
    const std::size_t body = body_at == std::string::npos ? 0 : got.size() - body_at - 4;
    check(body < size, "cut file: the whole file went out (" + std::to_string(body) + " bytes)");
    check(p.conn->closed(), "cut file: connection left open after a body of " + std::to_string(body) +
                            " of " + std::to_string(size) + " bytes");
    check(eof, "cut file: the client does not see the connection end");
    ::unlink(file.c_str());
}

} // namespace

int main()
{
    content_type_case();
    cut_file_case();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("http_send_file: all checks passed\n");
    return 0;
}
