#pragma once

#include "apostol/tcp.hpp"

#include <cstddef>
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
    bool feed(const char* data, std::size_t len);

    /// Human-readable description of the last error (valid only when feed()
    /// returned false).
    std::string_view error() const noexcept { return error_msg_; }

private:
    // llhttp parser state — allocated to keep llhttp out of public headers
    std::unique_ptr<llhttp_t>          parser_;
    std::unique_ptr<llhttp_settings_t> settings_;

    // Per-request accumulation
    HttpRequest   current_;
    std::string   current_field_;   // partially received header field name
    std::string   current_value_;   // partially received header field value
    bool          error_{false};
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
/// Call on_readable() each time the fd becomes readable (EPOLLIN).
class HttpConnection
{
public:
    using RequestHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

    explicit HttpConnection(TcpConnection conn);

    int fd() const noexcept { return conn_.fd(); }

    /// Read available data, parse HTTP, and call @p handler for each complete
    /// request.  Sends the response synchronously via send_response().
    /// Returns false when the connection should be closed (EOF or parse error).
    bool on_readable(RequestHandler handler);

    /// Write the serialized response to the socket.
    void send_response(const HttpResponse& resp);

    /// Transfer ownership of the underlying TCP connection out of this
    /// HttpConnection. After this call do not call on_readable() or
    /// send_response() — the HttpConnection is in an empty/released state.
    TcpConnection release_tcp();

private:
    TcpConnection conn_;
    HttpParser    parser_;
    bool          closed_{false};
};

} // namespace apostol
