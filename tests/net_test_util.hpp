// net_test_util.hpp — helpers shared by the tests that talk to a scripted peer
// on 127.0.0.1:
//
//   Server   a listener on an ephemeral port; every accepted connection is
//            handled by the test's function on a thread of its own
//   Conn     one accepted connection: bounded reads and writes, and (with
//            WITH_SSL) an upgrade to TLS in the server role
//   Identity a self-signed certificate with the subject alternative names the
//            test asks for, made at run time; Trust makes the client's
//            default trust store (SSL_CERT_FILE) hold it
//   ws_upgrade  the server half of a WebSocket opening handshake
//   run_until / settle   drive an EventLoop for a bounded time
//
// Header only; each test includes it.

#pragma once

#include "apostol/event_loop.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef WITH_SSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace nettest
{

using namespace std::chrono_literals;

enum class Rd { data, timeout, eof };

// ─── Conn ────────────────────────────────────────────────────────────────────

// One accepted connection (non-blocking underneath). Every wait is cut short
// when the server is stopping, so a handler never outlives its test.
class Conn
{
public:
    Conn(int fd, const std::atomic<bool>& stop) : fd_(fd), stop_(stop) {}
    ~Conn()
    {
#ifdef WITH_SSL
        if (ssl_)
            ::SSL_free(ssl_);
#endif
        if (fd_ >= 0)
            ::close(fd_);
    }
    Conn(const Conn&)            = delete;
    Conn& operator=(const Conn&) = delete;

    bool stopping() const { return stop_.load(); }

    // Wait for the socket; false on timeout or when stopping.
    bool wait(short events, int ms) const
    {
        for (int waited = 0; waited < ms; waited += 20) {
            if (stop_.load())
                return false;
            pollfd p{fd_, events, 0};
            if (::poll(&p, 1, 20) > 0)
                return true;
        }
        return false;
    }

    // Append what arrives to `out`. eof: the peer closed, or the server stops.
    Rd read(std::string& out, int ms)
    {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            char buf[65536];
#ifdef WITH_SSL
            if (ssl_) {
                const int k = ::SSL_read(ssl_, buf, sizeof buf);
                if (k > 0) {
                    out.append(buf, static_cast<std::size_t>(k));
                    return Rd::data;
                }
                const int e = ::SSL_get_error(ssl_, k);
                if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                    return Rd::eof;
            } else
#endif
            {
                const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
                if (n > 0) {
                    out.append(buf, static_cast<std::size_t>(n));
                    return Rd::data;
                }
                if (n == 0)
                    return Rd::eof;
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    return Rd::eof;
            }
            const auto left = ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - start).count());
            if (left <= 0)
                return Rd::timeout;
            if (stop_.load())
                return Rd::eof;
            wait(POLLIN, left < 20 ? left : 20);
        }
    }

    // Send everything within `ms`; false on a failure, a timeout or a stop.
    bool write(std::string_view data, int ms = 5000)
    {
        const auto start = std::chrono::steady_clock::now();
        std::size_t off  = 0;
        while (off < data.size()) {
            if (stop_.load())
                return false;
            const std::size_t chunk = std::min<std::size_t>(data.size() - off, 16384);
            bool want_wait          = false;
#ifdef WITH_SSL
            if (ssl_) {
                const int k = ::SSL_write(ssl_, data.data() + off, static_cast<int>(chunk));
                if (k > 0)
                    off += static_cast<std::size_t>(k);
                else {
                    const int e = ::SSL_get_error(ssl_, k);
                    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                        return false;
                    want_wait = true;
                }
            } else
#endif
            {
                const ssize_t n = ::send(fd_, data.data() + off, chunk, MSG_NOSIGNAL);
                if (n > 0)
                    off += static_cast<std::size_t>(n);
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    want_wait = true;
                else
                    return false;
            }
            if (want_wait) {
                const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (el > ms)
                    return false;
                wait(POLLOUT | POLLIN, 20);
            }
        }
        return true;
    }

    // Close now (the peer sees the end); the Conn is unusable afterwards.
    void close_now()
    {
#ifdef WITH_SSL
        if (ssl_) {
            ::SSL_free(ssl_);
            ssl_ = nullptr;
        }
#endif
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

#ifdef WITH_SSL
    // The handshake in the server role; false when the peer refuses.
    bool start_tls(SSL_CTX* ctx)
    {
        ssl_ = ::SSL_new(ctx);
        if (!ssl_)
            return false;
        ::SSL_set_fd(ssl_, fd_);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const int rc = ::SSL_accept(ssl_);
            if (rc == 1)
                return true;
            const int e = ::SSL_get_error(ssl_, rc);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                break;
            if (std::chrono::steady_clock::now() - start > 5s || stop_.load())
                break;
            wait(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 20);
        }
        ::SSL_free(ssl_);
        ssl_ = nullptr;
        return false;
    }
#endif

private:
    int                       fd_;
    const std::atomic<bool>&  stop_;
#ifdef WITH_SSL
    SSL* ssl_{nullptr};
#endif
};

// Read until `marker` is in `buf`; false on eof or when `ms` pass.
inline bool read_until(Conn& c, std::string& buf, std::string_view marker, int ms = 3000)
{
    const auto start = std::chrono::steady_clock::now();
    while (buf.find(marker) == std::string::npos) {
        const auto left = ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start).count());
        if (left <= 0 || c.read(buf, left) != Rd::data)
            return buf.find(marker) != std::string::npos;
    }
    return true;
}

// ─── Server ──────────────────────────────────────────────────────────────────

class Server
{
public:
    using Handler = std::function<void(Conn&, int index)>;

    explicit Server(Handler handler) : handler_(std::move(handler))
    {
        lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len     = sizeof a;
        if (lfd_ < 0 || ::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 64) != 0
            || ::getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return;
        port_     = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    ~Server()
    {
        stop_ = true;
        if (acceptor_.joinable())
            acceptor_.join();
        for (auto& w : workers_)
            w.join();
        if (lfd_ >= 0)
            ::close(lfd_);
    }
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }
    bool          ok() const { return port_ != 0; }
    int           accepted() const { return accepted_.load(); }
    bool          stopping() const { return stop_.load(); }

private:
    void accept_loop()
    {
        while (!stop_.load()) {
            pollfd p{lfd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            const int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0)
                continue;
            const int index = accepted_.fetch_add(1);
            workers_.emplace_back([this, fd, index] {
                Conn c(fd, stop_);
                try {
                    handler_(c, index);
                } catch (...) {
                }
            });
        }
    }

    Handler            handler_;
    int                lfd_{-1};
    std::uint16_t      port_{0};
    std::atomic<bool>  stop_{false};
    std::atomic<int>   accepted_{0};
    std::thread        acceptor_;
    std::vector<std::thread> workers_;   // touched by the acceptor, then by the destructor
};

// ─── EventLoop driving ───────────────────────────────────────────────────────

// Run the loop until `pred()` holds (polled every 10 ms) or `max_ms` pass.
template <class Pred>
bool run_until(apostol::EventLoop& loop, Pred pred, int max_ms)
{
    const auto start = std::chrono::steady_clock::now();
    const auto id    = loop.add_timer(10ms, [&] {
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (pred() || el >= max_ms)
            loop.stop();
    }, true);
    loop.run();
    loop.cancel_timer(id);
    return pred();
}

// Run the loop for `ms`, whatever happens.
inline void settle(apostol::EventLoop& loop, int ms)
{
    loop.add_timer(std::chrono::milliseconds(ms), [&loop] { loop.stop(); }, false);
    loop.run();
}

// ─── WebSocket server half ───────────────────────────────────────────────────

#ifdef WITH_SSL

inline std::string sha1_base64(const std::string& s)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    EVP_Digest(s.data(), s.size(), md, &n, EVP_sha1(), nullptr);
    unsigned char out[64]{};
    const int     k = EVP_EncodeBlock(out, md, static_cast<int>(n));
    return std::string(reinterpret_cast<char*>(out), static_cast<std::size_t>(k));
}

#endif

// Read the client's upgrade request; the Sec-WebSocket-Key it carries, or an
// empty string when none comes.
inline std::string read_ws_key(Conn& c)
{
    std::string req;
    if (!read_until(c, req, "\r\n\r\n"))
        return {};
    const std::string tag = "Sec-WebSocket-Key: ";
    const auto        p   = req.find(tag);
    if (p == std::string::npos)
        return {};
    const auto e = req.find("\r\n", p);
    return req.substr(p + tag.size(), e - p - tag.size());
}

#ifdef WITH_SSL

// The server half of the opening handshake: read the request, answer 101 with
// the right Sec-WebSocket-Accept (or, with good=false, a wrong one). False
// when no request comes.
inline bool ws_handshake(Conn& c, bool good = true)
{
    const std::string key = read_ws_key(c);
    if (key.empty())
        return false;
    const std::string accept = good ? sha1_base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
                                    : std::string("AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    return c.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
}

#endif

// ─── TLS identities ──────────────────────────────────────────────────────────

#ifdef WITH_SSL

// A self-signed certificate (also its own trust anchor) naming `san`, e.g.
// "DNS:localhost" or "IP:127.0.0.1", and the key; ctx() serves it.
class Identity
{
public:
    explicit Identity(const std::string& san)
    {
        key_.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
        cert_.reset(X509_new());
        X509* x = cert_.get();
        X509_set_version(x, 2);
        static std::atomic<long> serial{1000};
        ASN1_INTEGER_set(X509_get_serialNumber(x), serial.fetch_add(1));
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
        X509_set_pubkey(x, key_.get());
        X509_NAME* n = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("libapostol test"), -1, -1, 0);
        X509_set_issuer_name(x, n);

        X509V3_CTX v3;
        X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
        add_ext(x, &v3, NID_basic_constraints, "critical,CA:TRUE");
        add_ext(x, &v3, NID_subject_alt_name, san.c_str());
        X509_sign(x, key_.get(), EVP_sha256());

        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(bio, x);
        char* data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        pem_.assign(data, static_cast<std::size_t>(len));
        BIO_free(bio);

        ctx_.reset(SSL_CTX_new(TLS_server_method()));
        SSL_CTX_use_certificate(ctx_.get(), x);
        SSL_CTX_use_PrivateKey(ctx_.get(), key_.get());
    }

    SSL_CTX*           ctx() const { return ctx_.get(); }
    const std::string& pem() const { return pem_; }

private:
    static void add_ext(X509* x, X509V3_CTX* v3, int nid, const char* value)
    {
        X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, v3, nid, const_cast<char*>(value));
        if (e) {
            X509_add_ext(x, e, -1);
            X509_EXTENSION_free(e);
        }
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)>         cert_{nullptr, X509_free};
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>   ctx_{nullptr, SSL_CTX_free};
    std::string                                         pem_;
};

// The client side's trust: a temporary file holding `pem`, named by
// SSL_CERT_FILE. A TLS client reads it when its first handshake builds the
// context, so a test sets it before that and changes it between clients.
class Trust
{
public:
    explicit Trust(const std::string& pem)
    {
        char tmpl[] = "./apostol_trust_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0)
            return;
        std::size_t off = 0;
        while (off < pem.size()) {
            const ssize_t n = ::write(fd, pem.data() + off, pem.size() - off);
            if (n <= 0)
                break;
            off += static_cast<std::size_t>(n);
        }
        ::close(fd);
        char cwd[4096];
        if (::getcwd(cwd, sizeof cwd))
            path_ = std::string(cwd) + "/" + (tmpl + 2);
        ::setenv("SSL_CERT_FILE", path_.c_str(), 1);
    }
    ~Trust()
    {
        if (!path_.empty())
            ::unlink(path_.c_str());
    }
    Trust(const Trust&)            = delete;
    Trust& operator=(const Trust&) = delete;

private:
    std::string path_;
};

#endif // WITH_SSL

} // namespace nettest
