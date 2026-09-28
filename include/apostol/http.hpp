#pragma once

#include "apostol/tcp.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Forward-declare llhttp types to avoid polluting public headers
struct llhttp__internal_s;
using llhttp_t = llhttp__internal_s;
struct llhttp_settings_s;
using llhttp_settings_t = llhttp_settings_s;

namespace apostol
{

class EventLoop;  // forward declaration for HttpConnection async writes

// ─── HttpStatus ──────────────────────────────────────────────────────────────

enum class HttpStatus : int {
    switching_protocols     = 101,
    ok                      = 200,
    created                 = 201,
    no_content              = 204,
    moved_permanently       = 301,
    found                   = 302,
    not_modified            = 304,
    bad_request             = 400,
    unauthorized            = 401,
    forbidden               = 403,
    not_found               = 404,
    not_allowed             = 405,
    conflict                = 409,
    gone                    = 410,
    unprocessable_entity    = 422,
    too_many_requests       = 429,
    internal_server_error   = 500,
    not_implemented         = 501,
    bad_gateway             = 502,
    service_unavailable     = 503,
};

/// Return the standard reason phrase for a status code (e.g. "Not Found").
std::string_view status_text(HttpStatus s) noexcept;

/// The HttpStatus @p code names, or nothing when it names none of them.
///
/// The counterpart of status_text: that one turns a status into a phrase, this
/// one turns a number back into a status. A number that arrives from outside —
/// a database's error envelope, an upstream's status line — is not a status
/// until it is recognised as one, and casting it blindly produces a status line
/// this library has no reason phrase for.
std::optional<HttpStatus> status_from_code(int code) noexcept;

// ─── HttpRequest ─────────────────────────────────────────────────────────────

struct HttpRequest
{
    std::string method;   // "GET", "POST", …
    std::string path;     // "/api/v1/foo"  (query string stripped)
    std::string query;    // "bar=1&baz=2"  (raw, no leading '?')
    std::string version;  // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;  // order preserved
    std::vector<std::pair<std::string, std::string>> params;   // URL-decoded query params
    std::vector<std::pair<std::string, std::string>> cookies;  // parsed Cookie header
    std::string body;

    // Connection context — set by HttpConnection::on_readable(), read-only for handlers.
    std::string peer_ip;       // e.g. "127.0.0.1"
    uint16_t    peer_port{0};  // e.g. 41698
    int         socket_fd{-1}; // e.g. 26

    /// Opaque connection handle for deferred (async) responses.
    /// Set by start_http_server(); modules use it to send responses later.
    /// mutable because it's transport context, not request data.
    mutable std::shared_ptr<void> connection_ctx;

    /// Case-insensitive header lookup. Returns empty string when not found.
    std::string header(std::string_view name) const;

    /// True for HTTP/1.1 without "Connection: close";
    /// true for HTTP/1.0 only if "Connection: keep-alive" is set explicitly.
    bool keep_alive() const;

    /// URL-decoded query-param lookup. Returns @p def when not found.
    std::string param(std::string_view name, std::string_view def = "") const;

    /// Cookie lookup. Returns @p def when not found.
    std::string cookie(std::string_view name, std::string_view def = "") const;

    /// Value of the Content-Type header (empty if absent).
    std::string content_type() const { return header("Content-Type"); }

    /// Numeric value of Content-Length (0 if absent or non-numeric).
    std::size_t content_length() const;
};

// ─── HttpResponse ────────────────────────────────────────────────────────────

class HttpResponse
{
public:
    HttpResponse& set_status(int code, std::string text);

    /// Overload that takes a typed enum and fills in the standard reason phrase.
    HttpResponse& set_status(HttpStatus status);

    int status_code() const noexcept { return status_code_; }

    /// Headers as they will be written, in order, duplicates included.
    const std::vector<std::pair<std::string, std::string>>& headers() const noexcept
    {
        return headers_;
    }

    /// Replace existing header with the same name (case-sensitive).
    HttpResponse& set_header(std::string name, std::string value);

    /// Append a header without deduplication (use for Set-Cookie).
    HttpResponse& add_header(std::string name, std::string value);

    /// Remove all headers whose name matches @p name (case-sensitive).
    HttpResponse& del_header(std::string_view name);

    HttpResponse& set_body(std::string body, std::string content_type = "text/plain");

    /// For HEAD responses: body_ drives Content-Length but is not sent.
    HttpResponse& suppress_body() noexcept { suppress_body_ = true; return *this; }

    /// Add "Connection: close" to the response.
    HttpResponse& set_close(bool close = true);

    /// Append a Set-Cookie header with standard attributes.
    HttpResponse& set_cookie(std::string_view name,
                             std::string_view value,
                             std::string_view path      = "/",
                             int              max_age   = 0,
                             bool             http_only = true,
                             std::string_view same_site = "Lax",
                             bool             secure    = false,
                             std::string_view domain    = "");

    /// Mark the response as deferred — the handler will send it later via
    /// HttpConnection::send_response() obtained through req.connection_ctx.
    HttpResponse& set_deferred(bool d = true) noexcept { deferred_ = d; return *this; }
    bool is_deferred() const noexcept { return deferred_; }

    /// Reset to default state (200 OK, no headers, no body).
    HttpResponse& clear();

    /// Serialize to a complete HTTP/1.1 response string, including
    /// auto-computed Content-Length.
    std::string serialize() const;

private:
    int         status_code_{200};
    std::string status_text_{"OK"};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    bool        suppress_body_{false};
    bool        deferred_{false};
};

// ─── HttpParser ──────────────────────────────────────────────────────────────

/// Pure push-parser around llhttp. No I/O.
/// Feed raw bytes; for each complete request the handler is called once.
class HttpParser
{
public:
    using Handler = std::function<void(HttpRequest)>;

    HttpParser();
    ~HttpParser();

    HttpParser(const HttpParser&)            = delete;
    HttpParser& operator=(const HttpParser&) = delete;

    HttpParser(HttpParser&&) noexcept;
    HttpParser& operator=(HttpParser&&) noexcept;

    void set_handler(Handler h) { handler_ = std::move(h); }

    /// Feed @p len bytes. Returns false if the parser encountered an error.
    /// consumed() tells how many of them were parsed: fewer than @p len when
    /// the handler called hold(), or when llhttp paused after an Upgrade
    /// request (feed() returns false then, and malformed() is false).
    bool feed(const char* data, std::size_t len);

    /// Stop after the request being dispatched: called from the handler, it
    /// makes feed() return with the bytes after that request unparsed, so
    /// that the caller can answer it before reading the next one (T314).
    void hold() noexcept { hold_ = true; }

    /// Bytes of the last feed() that were parsed.
    std::size_t consumed() const noexcept { return consumed_; }

    /// Go on parsing after an Upgrade request that was answered over HTTP/1.1
    /// (h2c): RFC 9110 §7.8 lets a server ignore Upgrade. A refused WebSocket
    /// handshake does not come here — it closes after its answer (T621).
    /// Returns false unless feed() stopped on exactly that.
    bool resume_after_upgrade() noexcept;

    /// Human-readable description of the last error (valid only when feed()
    /// returned false).
    std::string_view error() const noexcept { return error_msg_; }

    /// The last feed() failed because the bytes are not a valid request —
    /// as opposed to llhttp pausing after an Upgrade request, which it reports
    /// the same way (feed() returns false) though nothing is wrong with it.
    bool malformed() const noexcept { return malformed_; }

private:
    // llhttp parser state — allocated to keep llhttp out of public headers
    std::unique_ptr<llhttp_t>          parser_;
    std::unique_ptr<llhttp_settings_t> settings_;

    // Per-request accumulation
    HttpRequest   current_;
    std::string   current_field_;   // partially received header field name
    std::string   current_value_;   // partially received header field value
    bool          error_{false};
    bool          malformed_{false};
    bool          hold_{false};
    std::size_t   consumed_{0};
    std::string   error_msg_;

    Handler handler_;

    void flush_header();  // move current_field_/value_ into current_.headers

    // llhttp C callbacks — must be static because they're called from C
    static int cb_on_url             (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_header_field    (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_header_value    (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_headers_complete(llhttp_t*);
    static int cb_on_body            (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_message_complete(llhttp_t*);
};

// ─── HttpClientResponse ──────────────────────────────────────────────────────

/// Parsed HTTP response (for client-side use).
struct HttpClientResponse
{
    int         status_code{0};
    std::string status_text;
    std::string version;   // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// Case-insensitive header lookup. Returns empty string when not found.
    std::string header(std::string_view name) const;

    /// Numeric value of Content-Length (0 if absent or non-numeric).
    std::size_t content_length() const;
};

// ─── HttpResponseParser ─────────────────────────────────────────────────────

/// Push-parser for HTTP responses (mirrors HttpParser but for HTTP_RESPONSE).
class HttpResponseParser
{
public:
    using Handler = std::function<void(HttpClientResponse)>;

    HttpResponseParser();
    ~HttpResponseParser();

    HttpResponseParser(const HttpResponseParser&)            = delete;
    HttpResponseParser& operator=(const HttpResponseParser&) = delete;

    HttpResponseParser(HttpResponseParser&&) noexcept;
    HttpResponseParser& operator=(HttpResponseParser&&) noexcept;

    void set_handler(Handler h) { handler_ = std::move(h); }

    /// The response answers a HEAD (or is otherwise known to carry no body):
    /// complete it at the end of the headers, whatever Content-Length says.
    /// A parser cannot tell this from the bytes — only the requester knows.
    void expect_no_body(bool v = true) noexcept { no_body_ = v; }

    /// Feed raw bytes. Returns false on parse error.
    bool feed(const char* data, std::size_t len);

    /// Human-readable error (valid only when feed() returned false).
    std::string_view error() const noexcept { return error_msg_; }

private:
    std::unique_ptr<llhttp_t>          parser_;
    std::unique_ptr<llhttp_settings_t> settings_;

    HttpClientResponse current_;
    std::string        current_field_;
    std::string        current_value_;
    bool               error_{false};
    bool               no_body_{false};
    std::string        error_msg_;

    Handler handler_;

    void flush_header();

    static int cb_on_status        (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_header_field  (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_header_value  (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_headers_complete(llhttp_t*);
    static int cb_on_body          (llhttp_t*, const char* at, std::size_t len);
    static int cb_on_message_complete(llhttp_t*);
};

// ─── HttpConnection ──────────────────────────────────────────────────────────

/// Owns a TcpConnection and an HttpParser.
/// Drive it with on_event() for every epoll event on its fd (as
/// Application::start_http_server does): it reads, dispatches one request at a
/// time, writes, and sets the fd's event mask itself. on_readable() remains
/// for a synchronous caller without an EventLoop; with one, calling it while an
/// answer is owed means "the peer left" and closes the connection.
///
/// Supports async writes: if a write cannot complete immediately, the remainder
/// is buffered and flushed via EPOLLOUT (requires EventLoop* passed to ctor).
/// send_file() uses sendfile(2) for zero-copy file serving.
class HttpConnection
{
public:
    using RequestHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

    /// @param loop  If non-null, enables async write buffering + sendfile.
    explicit HttpConnection(TcpConnection conn, EventLoop* loop = nullptr);
    ~HttpConnection();

    HttpConnection(const HttpConnection&)            = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
    HttpConnection(HttpConnection&&)                 = delete;
    HttpConnection& operator=(HttpConnection&&)      = delete;

    int fd() const noexcept { return conn_.fd(); }
    bool closed() const noexcept { return closed_; }

    /// Read available data, parse HTTP, and call @p handler for each complete
    /// request.  Sends the response synchronously via send_response().
    /// Returns false when the connection should be closed (EOF or parse error).
    bool on_readable(RequestHandler handler);

    /// Handle one epoll event on this connection's fd: drain pending writes on
    /// EPOLLOUT, then read and dispatch requests. The whole per-event step of
    /// the server's I/O callback, so that a test drives the path the server
    /// runs. Returns false when the caller should remove the fd from the loop;
    /// otherwise the caller re-arms it.
    bool on_event(uint32_t events, const RequestHandler& handler);

    /// Write the serialized response to the socket.
    /// If EventLoop is available and the write would block, the remainder is
    /// buffered and flushed asynchronously via EPOLLOUT.
    void send_response(const HttpResponse& resp);

    /// Send a file directly from disk using sendfile(2) zero-copy.
    /// Constructs and sends HTTP 200 headers, then streams file data.
    /// Requires EventLoop* (falls back to buffered read if null).
    void send_file(const std::string& path, std::string_view mime_type);

    /// Drain pending writes. Call on EPOLLOUT events.
    /// Returns true if there are still pending writes.
    bool on_writable();

    /// True if there is buffered data or an active file transfer.
    bool has_pending_writes() const noexcept;

    /// Transfer ownership of the underlying TCP connection out of this
    /// HttpConnection. After this call do not call on_readable() or
    /// send_response() — the HttpConnection is in an empty/released state.
    TcpConnection release_tcp();

private:
    TcpConnection conn_;
    HttpParser    parser_;
    EventLoop*    loop_{nullptr};
    bool          closed_{false};

    // One request in work per connection (T314; RFC 9112 §9.3.2 wants answers
    // in request order, and a module answering later through connection_ctx
    // does not say which request it answers). While an answer is owed —
    // deferred and not yet sent, or a file still being written — the parser
    // holds after the request, the bytes read past it wait in pending_in_,
    // and the socket is not read: EPOLLIN is off, EPOLLRDHUP stays on so that
    // a client who left is noticed (there is no idle timeout, and an answer
    // may never come). The answer arriving from a module's callback gives the
    // reading back; the next request is parsed in this connection's own I/O
    // callback, never inside the module's.
    bool          awaiting_{false};          // deferred answer not yet started
    bool          dispatching_{false};
    bool          answered_in_dispatch_{false};
    bool          close_after_send_{false};  // the request in work asked to close
    bool          stop_reading_{false};      // ... so nothing after it is parsed
    std::string   pending_in_;               // read, not yet parsed
    uint32_t      mask_;                     // events last set on the loop

    /// An answer is owed: nothing further may be parsed.
    bool owed() const noexcept { return awaiting_ || file_fd_ >= 0; }

    /// Bytes already read wait to be parsed and nothing stops them.
    bool resumable() const noexcept;

    void response_started() noexcept;

    /// An answer may have gone out whole: close if the request asked for it,
    /// otherwise give the reading back. Not during a dispatch — on_readable
    /// settles the connection itself at its end.
    void after_response();

    /// Feed bytes to the parser; false when the connection is to be closed.
    bool feed_input(const char* data, std::size_t len);

    /// Half-close and read what the client already sent, so that close()
    /// does not answer unread input with RST.
    void shutdown_and_drain() noexcept;

    // Async write buffer
    std::string   write_buf_;
    std::size_t   write_pos_{0};

    // sendfile(2) state
    int           file_fd_{-1};
    off_t         file_offset_{0};
    std::size_t   file_remaining_{0};

    bool drain_buffer();
    bool drain_file();
    void update_interest();
};

} // namespace apostol
