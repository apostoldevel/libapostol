#include "apostol/http.hpp"
#include "apostol/event_loop.hpp"

#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fmt/format.h>

#include <sys/socket.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

namespace apostol
{

// ─── Internal helpers ─────────────────────────────────────────────────────────

static int hex_val(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static std::string url_decode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::vector<std::pair<std::string,std::string>> parse_query(std::string_view q)
{
    std::vector<std::pair<std::string,std::string>> result;
    while (!q.empty()) {
        auto amp  = q.find('&');
        auto part = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!part.empty()) {
            auto eq = part.find('=');
            if (eq != std::string_view::npos)
                result.emplace_back(url_decode(part.substr(0, eq)),
                                    url_decode(part.substr(eq + 1)));
            else
                result.emplace_back(url_decode(part), "");
        }
        if (amp == std::string_view::npos) break;
        q = q.substr(amp + 1);
    }
    return result;
}

static std::vector<std::pair<std::string,std::string>> parse_cookies(std::string_view v)
{
    std::vector<std::pair<std::string,std::string>> result;
    while (!v.empty()) {
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        auto semi = v.find(';');
        auto part = (semi != std::string_view::npos) ? v.substr(0, semi) : v;
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (!part.empty()) {
            auto eq = part.find('=');
            if (eq != std::string_view::npos)
                result.emplace_back(std::string(part.substr(0, eq)),
                                    std::string(part.substr(eq + 1)));
            else
                result.emplace_back(std::string(part), "");
        }
        if (semi == std::string_view::npos) break;
        v = v.substr(semi + 1);
    }
    return result;
}

// ─── HttpStatus ───────────────────────────────────────────────────────────────

std::string_view status_text(HttpStatus s) noexcept
{
    switch (s) {
        case HttpStatus::switching_protocols:  return "Switching Protocols";
        case HttpStatus::ok:                   return "OK";
        case HttpStatus::created:              return "Created";
        case HttpStatus::no_content:           return "No Content";
        case HttpStatus::moved_permanently:    return "Moved Permanently";
        case HttpStatus::found:                return "Found";
        case HttpStatus::not_modified:         return "Not Modified";
        case HttpStatus::bad_request:          return "Bad Request";
        case HttpStatus::unauthorized:         return "Unauthorized";
        case HttpStatus::forbidden:            return "Forbidden";
        case HttpStatus::not_found:            return "Not Found";
        case HttpStatus::not_allowed:          return "Method Not Allowed";
        case HttpStatus::conflict:             return "Conflict";
        case HttpStatus::gone:                 return "Gone";
        case HttpStatus::unprocessable_entity: return "Unprocessable Entity";
        case HttpStatus::too_many_requests:    return "Too Many Requests";
        case HttpStatus::internal_server_error: return "Internal Server Error";
        case HttpStatus::not_implemented:      return "Not Implemented";
        case HttpStatus::bad_gateway:          return "Bad Gateway";
        case HttpStatus::service_unavailable:  return "Service Unavailable";
    }
    return "Unknown";
}

std::optional<HttpStatus> status_from_code(int code) noexcept
{
    switch (static_cast<HttpStatus>(code)) {
        case HttpStatus::switching_protocols:
        case HttpStatus::ok:
        case HttpStatus::created:
        case HttpStatus::no_content:
        case HttpStatus::moved_permanently:
        case HttpStatus::found:
        case HttpStatus::not_modified:
        case HttpStatus::bad_request:
        case HttpStatus::unauthorized:
        case HttpStatus::forbidden:
        case HttpStatus::not_found:
        case HttpStatus::not_allowed:
        case HttpStatus::conflict:
        case HttpStatus::gone:
        case HttpStatus::unprocessable_entity:
        case HttpStatus::too_many_requests:
        case HttpStatus::internal_server_error:
        case HttpStatus::not_implemented:
        case HttpStatus::bad_gateway:
        case HttpStatus::service_unavailable:
            return static_cast<HttpStatus>(code);
    }
    return std::nullopt;
}

// ─── HttpRequest ─────────────────────────────────────────────────────────────

std::string HttpRequest::header(std::string_view name) const
{
    for (const auto& [k, v] : headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(),
                       [](unsigned char a, unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       }))
        {
            return v;
        }
    }
    return {};
}

bool HttpRequest::keep_alive() const
{
    auto conn = header("Connection");
    std::transform(conn.begin(), conn.end(), conn.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (version == "HTTP/1.1")
        return conn != "close";
    // HTTP/1.0: keep-alive only if explicitly requested
    return conn == "keep-alive";
}

std::string HttpRequest::param(std::string_view name, std::string_view def) const
{
    for (const auto& [k, v] : params)
        if (k == name) return v;
    return std::string(def);
}

std::string HttpRequest::cookie(std::string_view name, std::string_view def) const
{
    for (const auto& [k, v] : cookies)
        if (k == name) return v;
    return std::string(def);
}

std::size_t HttpRequest::content_length() const
{
    auto h = header("Content-Length");
    if (h.empty()) return 0;
    std::size_t val = 0;
    for (char c : h) {
        if (c < '0' || c > '9') return 0;
        val = val * 10 + static_cast<std::size_t>(c - '0');
    }
    return val;
}

// ─── HttpResponse ────────────────────────────────────────────────────────────

// ─── header_safe / header_name_ok ────────────────────────────────────────────
//
// A response header ends at the first control byte, and a header whose name is not
// a token is not written at all.
//
// Values reach the response from query strings, database rows and upstream replies.
// A CR or LF in one of them ends the header early, and everything after it is read
// by the client as a further header — or, past a blank line, as a further response
// on the same connection. That is response splitting: a redirect built from a
// request parameter becomes a way to set a cookie on this origin, or to serve a
// body of the caller's choosing from it.
//
// Truncating rather than stripping is deliberate. Stripping the newlines out of
// "x\r\nSet-Cookie: …" leaves "xSet-Cookie: …" — still the caller's text sitting in
// our header, merely no longer on its own line. Truncating leaves "x", which is
// what the value was before the injection began.
//
// Nothing legitimate is lost: obsolete line folding is the only construct that ever
// put a CR LF inside a value, and RFC 9110 §5.5 forbids sending it.
//
namespace {

std::string header_safe(std::string value)
{
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == 0x7f || (c < 0x20 && c != '\t')) {
            value.resize(i);
            break;
        }
    }
    return value;
}

bool header_name_ok(const std::string& name)
{
    if (name.empty())
        return false;
    // RFC 9110 §5.1: a field name is a token. Anything else — a space, a colon, a
    // control byte — makes the line unparsable, so the header is dropped rather
    // than written and left for the peer to misread.
    for (const unsigned char c : name)
        if (c <= 0x20 || c >= 0x7f || c == ':')
            return false;
    return true;
}

} // namespace

HttpResponse& HttpResponse::set_status(int code, std::string text)
{
    status_code_ = code;
    // The reason phrase shares the status line with nothing that would contain it.
    status_text_ = header_safe(std::move(text));
    return *this;
}

HttpResponse& HttpResponse::set_status(HttpStatus status)
{
    status_code_ = static_cast<int>(status);
    status_text_ = std::string(apostol::status_text(status));
    return *this;
}

HttpResponse& HttpResponse::set_header(std::string name, std::string value)
{
    if (!header_name_ok(name))
        return *this;

    value = header_safe(std::move(value));

    // Replace existing header if same name
    for (auto& [k, v] : headers_) {
        if (k == name) { v = std::move(value); return *this; }
    }
    headers_.emplace_back(std::move(name), std::move(value));
    return *this;
}

HttpResponse& HttpResponse::add_header(std::string name, std::string value)
{
    if (!header_name_ok(name))
        return *this;

    headers_.emplace_back(std::move(name), header_safe(std::move(value)));
    return *this;
}

HttpResponse& HttpResponse::del_header(std::string_view name)
{
    headers_.erase(
        std::remove_if(headers_.begin(), headers_.end(),
                       [name](const auto& kv) { return kv.first == name; }),
        headers_.end());
    return *this;
}

HttpResponse& HttpResponse::set_body(std::string body, std::string content_type)
{
    body_ = std::move(body);
    set_header("Content-Type", std::move(content_type));
    return *this;
}

HttpResponse& HttpResponse::set_close(bool close)
{
    if (close)
        set_header("Connection", "close");
    return *this;
}

HttpResponse& HttpResponse::set_cookie(std::string_view name,
                                        std::string_view value,
                                        std::string_view path,
                                        int              max_age,
                                        bool             http_only,
                                        std::string_view same_site,
                                        bool             secure,
                                        std::string_view domain)
{
    std::string cookie_val = fmt::format("{}={}", name, value);
    if (!path.empty())
        cookie_val += fmt::format("; Path={}", path);
    if (max_age > 0)
        cookie_val += fmt::format("; Max-Age={}", max_age);
    else if (max_age < 0)
        cookie_val += "; Max-Age=0";
    if (!domain.empty())
        cookie_val += fmt::format("; Domain={}", domain);
    if (http_only)
        cookie_val += "; HttpOnly";
    if (!same_site.empty())
        cookie_val += fmt::format("; SameSite={}", same_site);
    if (secure)
        cookie_val += "; Secure";
    return add_header("Set-Cookie", std::move(cookie_val));
}

HttpResponse& HttpResponse::clear()
{
    status_code_   = 200;
    status_text_   = "OK";
    headers_.clear();
    body_.clear();
    suppress_body_ = false;
    deferred_      = false;
    return *this;
}

std::string HttpResponse::serialize() const
{
    std::string out;
    out.reserve(256 + body_.size());

    out += fmt::format("HTTP/1.1 {} {}\r\n", status_code_, status_text_);

    // Write explicit headers first
    for (const auto& [k, v] : headers_)
        out += fmt::format("{}: {}\r\n", k, v);

    // Always include Content-Length — from the body, which is what goes out.
    // The one exception is a suppressed body carrying its own header: a HEAD
    // answer relayed from an upstream names the size of the resource, and the
    // body here is empty by definition, so the caller's header is the truth.
    bool has_content_length = false;
    if (suppress_body_)
        for (const auto& [k, v] : headers_)
            if (k.size() == 14 && strncasecmp(k.c_str(), "Content-Length", 14) == 0) {
                has_content_length = true;
                break;
            }
    if (!has_content_length)
        out += fmt::format("Content-Length: {}\r\n", body_.size());

    out += "\r\n";
    if (!suppress_body_)
        out += body_;

    return out;
}

// ─── HttpClientResponse ──────────────────────────────────────────────────────

std::string HttpClientResponse::header(std::string_view name) const
{
    for (const auto& [k, v] : headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(),
                       [](unsigned char a, unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       }))
        {
            return v;
        }
    }
    return {};
}

std::size_t HttpClientResponse::content_length() const
{
    auto h = header("Content-Length");
    if (h.empty()) return 0;
    std::size_t val = 0;
    for (char c : h) {
        if (c < '0' || c > '9') return 0;
        val = val * 10 + static_cast<std::size_t>(c - '0');
    }
    return val;
}

// ─── HttpResponseParser — llhttp callbacks ───────────────────────────────────

static HttpResponseParser* resp_self(llhttp_t* p)
{
    return static_cast<HttpResponseParser*>(p->data);
}

int HttpResponseParser::cb_on_status(llhttp_t* p, const char* at, std::size_t len)
{
    resp_self(p)->current_.status_text.append(at, len);
    return 0;
}

int HttpResponseParser::cb_on_header_field(llhttp_t* p, const char* at, std::size_t len)
{
    auto* rp = resp_self(p);
    if (!rp->current_field_.empty())
        rp->flush_header();
    rp->current_field_.append(at, len);
    return 0;
}

int HttpResponseParser::cb_on_header_value(llhttp_t* p, const char* at, std::size_t len)
{
    resp_self(p)->current_value_.append(at, len);
    return 0;
}

int HttpResponseParser::cb_on_headers_complete(llhttp_t* p)
{
    auto* rp = resp_self(p);

    if (!rp->current_field_.empty())
        rp->flush_header();

    rp->current_.status_code = static_cast<int>(llhttp_get_status_code(p));

    rp->current_.version = fmt::format("HTTP/{}.{}",
        llhttp_get_http_major(p), llhttp_get_http_minor(p));

    // 1 tells llhttp there is no body to wait for (a HEAD answer announces
    // the resource's length and then sends nothing).
    return rp->no_body_ ? 1 : 0;
}

int HttpResponseParser::cb_on_body(llhttp_t* p, const char* at, std::size_t len)
{
    resp_self(p)->current_.body.append(at, len);
    return 0;
}

int HttpResponseParser::cb_on_message_complete(llhttp_t* p)
{
    auto* rp = resp_self(p);
    if (rp->handler_)
        rp->handler_(std::move(rp->current_));

    rp->current_       = {};
    rp->current_field_ = {};
    rp->current_value_ = {};
    return 0;
}

void HttpResponseParser::flush_header()
{
    current_.headers.emplace_back(std::move(current_field_),
                                   std::move(current_value_));
    current_field_.clear();
    current_value_.clear();
}

// ─── HttpResponseParser — constructor / destructor ───────────────────────────

HttpResponseParser::HttpResponseParser()
    : parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
{
    llhttp_settings_init(settings_.get());

    settings_->on_status           = cb_on_status;
    settings_->on_header_field     = cb_on_header_field;
    settings_->on_header_value     = cb_on_header_value;
    settings_->on_headers_complete = cb_on_headers_complete;
    settings_->on_body             = cb_on_body;
    settings_->on_message_complete = cb_on_message_complete;

    llhttp_init(parser_.get(), HTTP_RESPONSE, settings_.get());
    parser_->data = this;
}

HttpResponseParser::~HttpResponseParser() = default;

HttpResponseParser::HttpResponseParser(HttpResponseParser&& o) noexcept
    : parser_(std::move(o.parser_))
    , settings_(std::move(o.settings_))
    , current_(std::move(o.current_))
    , current_field_(std::move(o.current_field_))
    , current_value_(std::move(o.current_value_))
    , no_body_(o.no_body_)
    , handler_(std::move(o.handler_))
{
    if (parser_)
        parser_->data = this;
}

HttpResponseParser& HttpResponseParser::operator=(HttpResponseParser&& o) noexcept
{
    if (this != &o) {
        parser_        = std::move(o.parser_);
        settings_      = std::move(o.settings_);
        current_       = std::move(o.current_);
        current_field_ = std::move(o.current_field_);
        current_value_ = std::move(o.current_value_);
        no_body_       = o.no_body_;
        handler_       = std::move(o.handler_);
        if (parser_)
            parser_->data = this;
    }
    return *this;
}

bool HttpResponseParser::feed(const char* data, std::size_t len)
{
    if (error_)
        return false;

    llhttp_errno_t err = llhttp_execute(parser_.get(), data, len);
    if (err != HPE_OK) {
        error_     = true;
        error_msg_ = llhttp_errno_name(err);
        return false;
    }
    return true;
}

// ─── HttpParser — llhttp callbacks ────────────────────────────────────────────

static HttpParser* self(llhttp_t* p)
{
    return static_cast<HttpParser*>(p->data);
}

int HttpParser::cb_on_url(llhttp_t* p, const char* at, std::size_t len)
{
    self(p)->current_.path.append(at, len);
    return 0;
}

int HttpParser::cb_on_header_field(llhttp_t* p, const char* at, std::size_t len)
{
    HttpParser* hp = self(p);
    // If we already have a value buffered, the previous field/value pair is done
    if (!hp->current_value_.empty() || (!hp->current_field_.empty() && hp->current_value_.empty())) {
        if (!hp->current_field_.empty())
            hp->flush_header();
    }
    hp->current_field_.append(at, len);
    return 0;
}

int HttpParser::cb_on_header_value(llhttp_t* p, const char* at, std::size_t len)
{
    self(p)->current_value_.append(at, len);
    return 0;
}

int HttpParser::cb_on_headers_complete(llhttp_t* p)
{
    HttpParser* hp = self(p);

    // Flush last pending header
    if (!hp->current_field_.empty())
        hp->flush_header();

    // Method string
    hp->current_.method = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(p)));

    // Version
    hp->current_.version = fmt::format("HTTP/{}.{}",
        llhttp_get_http_major(p), llhttp_get_http_minor(p));

    // Split path and query string (accumulated in path during cb_on_url)
    {
        auto& path = hp->current_.path;
        auto q = path.find('?');
        if (q != std::string::npos) {
            hp->current_.query  = path.substr(q + 1);
            path                = path.substr(0, q);
            hp->current_.params = parse_query(hp->current_.query);
        }
    }

    // Parse Cookie header
    auto cookie_hdr = hp->current_.header("Cookie");
    if (!cookie_hdr.empty())
        hp->current_.cookies = parse_cookies(cookie_hdr);

    return 0;
}

int HttpParser::cb_on_body(llhttp_t* p, const char* at, std::size_t len)
{
    self(p)->current_.body.append(at, len);
    return 0;
}

int HttpParser::cb_on_message_complete(llhttp_t* p)
{
    HttpParser* hp = self(p);
    if (hp->handler_)
        hp->handler_(std::move(hp->current_));

    // Reset for the next request (keep-alive / pipelining)
    hp->current_       = {};
    hp->current_field_ = {};
    hp->current_value_ = {};

    // hold(): stop here; llhttp resumes after this message on the next execute.
    if (hp->hold_) {
        hp->hold_ = false;
        return HPE_PAUSED;
    }
    return 0;
}

void HttpParser::flush_header()
{
    current_.headers.emplace_back(std::move(current_field_),
                                  std::move(current_value_));
    current_field_.clear();
    current_value_.clear();
}

// ─── HttpParser — constructor / destructor ────────────────────────────────────

HttpParser::HttpParser()
    : parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
{
    llhttp_settings_init(settings_.get());

    settings_->on_url              = cb_on_url;
    settings_->on_header_field     = cb_on_header_field;
    settings_->on_header_value     = cb_on_header_value;
    settings_->on_headers_complete = cb_on_headers_complete;
    settings_->on_body             = cb_on_body;
    settings_->on_message_complete = cb_on_message_complete;

    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = this;
}

HttpParser::~HttpParser() = default;

HttpParser::HttpParser(HttpParser&& o) noexcept
    : parser_(std::move(o.parser_))
    , settings_(std::move(o.settings_))
    , current_(std::move(o.current_))
    , current_field_(std::move(o.current_field_))
    , current_value_(std::move(o.current_value_))
    , handler_(std::move(o.handler_))
{
    // Fix userdata pointer — the parser stores a raw pointer to this
    if (parser_)
        parser_->data = this;
}

HttpParser& HttpParser::operator=(HttpParser&& o) noexcept
{
    if (this != &o) {
        parser_        = std::move(o.parser_);
        settings_      = std::move(o.settings_);
        current_       = std::move(o.current_);
        current_field_ = std::move(o.current_field_);
        current_value_ = std::move(o.current_value_);
        handler_       = std::move(o.handler_);
        if (parser_)
            parser_->data = this;
    }
    return *this;
}

bool HttpParser::feed(const char* data, std::size_t len)
{
    consumed_ = 0;
    if (error_)
        return false;

    llhttp_errno_t err = llhttp_execute(parser_.get(), data, len);
    if (err == HPE_PAUSED) {
        // hold() from the handler: the request is dispatched, the rest waits.
        consumed_ = static_cast<std::size_t>(llhttp_get_error_pos(parser_.get()) - data);
        llhttp_resume(parser_.get());
        return true;
    }
    if (err != HPE_OK) {
        error_    = true;
        // HPE_PAUSED_UPGRADE is llhttp stopping after any request that carries
        // Upgrade (a WebSocket handshake, h2c, CONNECT) — the request itself
        // was fine and has been dispatched.
        malformed_ = err != HPE_PAUSED_UPGRADE;
        error_msg_ = llhttp_errno_name(err);
        if (!malformed_)
            consumed_ = static_cast<std::size_t>(llhttp_get_error_pos(parser_.get()) - data);
        return false;
    }
    consumed_ = len;
    return true;
}

bool HttpParser::resume_after_upgrade() noexcept
{
    if (!error_ || malformed_)
        return false;
    llhttp_resume_after_upgrade(parser_.get());
    error_ = false;
    error_msg_.clear();
    return true;
}

// A request asking for the WebSocket protocol — by its Upgrade header alone,
// complete handshake or not: a handshake refused for a missing key is refused
// all the same, and its client is just as unlikely to send anything else.
static bool wants_websocket(const HttpRequest& req)
{
    std::string up = req.header("Upgrade");
    std::transform(up.begin(), up.end(), up.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return up.find("websocket") != std::string::npos;
}

// ─── HttpConnection ───────────────────────────────────────────────────────────

HttpConnection::HttpConnection(TcpConnection conn, EventLoop* loop)
    : conn_(std::move(conn))
    , loop_(loop)
    , mask_(EPOLLIN | EPOLLRDHUP)   // what start_http_server registers
{}

HttpConnection::~HttpConnection()
{
    if (file_fd_ >= 0)
        ::close(file_fd_);
}

bool HttpConnection::on_readable(RequestHandler handler)
{
    if (closed_)
        return false;

    // Not reading, yet called: the socket's EPOLLIN is off, so what brought us
    // here is the peer — EPOLLRDHUP, EPOLLHUP, EPOLLERR. It left under an
    // answer still owed (or before the last one went out): close, as nginx
    // does with a client that aborts. The answer, should it come, finds the
    // connection closed and goes nowhere. Keeping the socket for it instead has
    // no bound: a cancelled query never calls back, and there is no idle timer.
    if (owed() || stop_reading_) {
        closed_ = true;
        return false;
    }

    dispatching_ = true;
    struct DispatchGuard {
        bool& flag;
        ~DispatchGuard() { flag = false; }
    } dispatch_guard{dispatching_};

    // Set the handler so the parser calls it per-request
    parser_.set_handler([&](HttpRequest req) {
        req.peer_ip   = conn_.peer_address();
        req.peer_port = conn_.peer_port();
        req.socket_fd = conn_.fd();
        HttpResponse resp;
        answered_in_dispatch_ = false;
        handler(req, resp);
        // A WebSocket handshake the handler did not take (release_tcp() sets
        // closed_) was refused — 401, 403, 400, now or deferred. Its client
        // waits for 101 or for the close, never sends another request, and
        // there is no idle timer: kept alive, the socket stays until the
        // client goes, and a station retrying with a wrong name holds one fd
        // per attempt (T621). Close once the refusal is out, as before T314.
        // Only for "Upgrade: websocket" — any other Upgrade (h2c from curl
        // --http2) is an ordinary request answered over HTTP/1.1, and its
        // connection stays open.
        const bool ws_refused = !closed_ && wants_websocket(req);
        if (ws_refused && !resp.is_deferred())
            resp.set_close(true);
        // Skip response if the handler upgraded to WebSocket (release_tcp() was called)
        // or if the handler marked it as deferred (async PG response)
        if (!closed_ && !resp.is_deferred())
            send_response(resp);
        if (resp.is_deferred() && !answered_in_dispatch_)
            awaiting_ = true;
        if (!req.keep_alive() || ws_refused) {
            close_after_send_ = true;
            stop_reading_     = true;   // RFC 9112 §9.6: nothing after it is processed
        }
        if (closed_ || owed() || stop_reading_)
            parser_.hold();
    });

    // What an earlier hold left unparsed goes first.
    if (!pending_in_.empty()) {
        std::string in = std::move(pending_in_);
        pending_in_.clear();
        if (!feed_input(in.data(), in.size()))
            return false;
    }

    char buf[8192];

    while (!closed_ && !owed() && !stop_reading_ && pending_in_.empty()) {
        ssize_t n = conn_.read(buf, sizeof(buf));

        if (n == 0) {
            // EOF — peer closed
            closed_ = true;
            return false;
        }

        if (n < 0) {
            // EAGAIN — no more data right now
            break;
        }

        if (!feed_input(buf, static_cast<std::size_t>(n)))
            return false;
    }

    if (closed_)
        return false;   // released for WebSocket upgrade, or a write failed

    // The last request asked to close and its answer is out whole.
    if (close_after_send_ && !owed() && !has_pending_writes()) {
        shutdown_and_drain();
        closed_ = true;
        return false;
    }

    update_interest();
    return true;
}

bool HttpConnection::feed_input(const char* data, std::size_t len)
{
    while (len > 0 && !closed_) {
        const bool ok   = parser_.feed(data, len);
        const auto used = parser_.consumed();
        data += used;
        len  -= used;

        if (!ok) {
            if (!parser_.malformed()) {
                // llhttp stops after any Upgrade request it was not told to hold
                // on — in practice "Upgrade: h2c" from curl --http2, answered at
                // once over HTTP/1.1: the connection stays HTTP/1.1. A WebSocket
                // handshake, taken (closed_) or refused (stop_reading_, see the
                // dispatch above), is held, and hold() wins over the upgrade
                // pause: it does not come here.
                if (closed_)
                    return true;
                parser_.resume_after_upgrade();
                if (owed() || stop_reading_) {
                    if (!stop_reading_)
                        pending_in_.assign(data, len);
                    return true;
                }
                continue;
            }

            // A request this parser cannot read — raw UTF-8 in the target, a
            // broken header line — used to end in a closed socket with no answer
            // at all. The client saw "empty reply", a proxy in front turned it
            // into 502, and 502 reads as "the server is down" when the fault is
            // the request's. RFC 9112 §3: a server that receives an invalid
            // request-line SHOULD respond with 400. The body is RFC 9457 with
            // type about:blank: routing has not happened, so no module's shape
            // applies. Not after a WebSocket upgrade: the socket belongs to the
            // WebSocket side then (release_tcp() set closed_).
            //
            // Nothing is owed here: the parser holds after a request whose
            // answer is, so a request behind it is read only once that answer
            // is out — and the 400 takes its place in line.
            if (!closed_ && !owed()) {
                // The llhttp error name is an identifier (HPE_INVALID_URL…):
                // nothing in it needs escaping.
                const std::string body = fmt::format(
                    "{{\"type\":\"about:blank\",\"title\":\"Bad Request\",\"status\":400,"
                    "\"detail\":\"the request could not be parsed ({})\"}}",
                    parser_.error());
                HttpResponse r;
                r.set_status(HttpStatus::bad_request)
                 .set_header("Connection", "close")
                 .set_body(body, "application/problem+json");
                send_response(r);
                if (closed_)
                    return false;   // the write failed

                // Close like any "Connection: close" answer: nothing after it is
                // read, and the connection closes once the 400 is out whole —
                // at the end of on_readable(), or from on_writable() when it sits
                // behind the tail of an earlier answer still in write_buf_.
                // Both paths half-close and drain first, so the close() does not
                // find unread input and answer it with RST. Closing here instead
                // dropped that tail and the 400 with it.
                close_after_send_ = true;
                stop_reading_     = true;
                return true;
            }
            closed_ = true;
            return false;
        }

        if (len > 0) {
            // hold(): the request in work is owed an answer or asked to close.
            // After a close the rest is dropped — RFC 9112 §9.6.
            if (!stop_reading_)
                pending_in_.assign(data, len);
            return true;
        }
    }
    return true;
}

bool HttpConnection::on_event(uint32_t events, const RequestHandler& handler)
{
    // Whatever throws here gets the connection dropped by the caller
    // (remove_io). Closed here as well: an answer arriving later would
    // otherwise try to set the mask of an fd the loop no longer has.
    try {
        // Drain pending async writes (sendfile, buffered responses)
        if (events & EPOLLOUT)
            on_writable();

        if (closed_)
            return false;

        // EPOLLHUP and EPOLLERR arrive whatever the mask says: under LT, left
        // unread they would fire again at once, round after round.
        if (!(events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) && !resumable())
            return true;

        return on_readable(handler);
    } catch (...) {
        closed_ = true;
        throw;
    }
}

void HttpConnection::shutdown_and_drain() noexcept
{
    // Close the way a server that answered should: half-close, then take what
    // the client already sent. A close() that finds unread input answers with
    // RST, and a client that gets RST drops what it has not read yet — the
    // tail of our answer. Input is left unread after a "Connection: close"
    // request with more behind it (RFC 9112 §9.6), or after a bad request.
    // Bounded: a client still streaming is not waited for.
    ::shutdown(conn_.fd(), SHUT_WR);
    char sink[8192];
    for (int i = 0; i < 64 && conn_.read(sink, sizeof(sink)) > 0; ++i) {}
}

bool HttpConnection::resumable() const noexcept
{
    return !closed_ && !owed() && !stop_reading_ && !pending_in_.empty();
}

void HttpConnection::response_started() noexcept
{
    // During a dispatch this is the current request's answer going out at
    // once; afterwards it is the deferred one arriving.
    if (dispatching_)
        answered_in_dispatch_ = true;
    else
        awaiting_ = false;
}

void HttpConnection::after_response()
{
    if (dispatching_ || closed_)
        return;

    if (close_after_send_ && !owed() && !has_pending_writes()) {
        close_after_send_ = false;
        shutdown_and_drain();
        closed_ = true;
        if (loop_)
            loop_->remove_io(conn_.fd());
        return;
    }

    // Reading back — and, with bytes already waiting in pending_in_, EPOLLOUT
    // as the wake-up: a writable socket reports it on the next turn of the
    // loop, where the I/O callback parses them. Not from here: this runs inside
    // a module's callback, and the next request is not to be dispatched there.
    update_interest();
}

void HttpConnection::send_response(const HttpResponse& resp)
{
    response_started();

    if (closed_) return;

    std::string data = resp.serialize();

    // If there are already pending writes, just append
    if (write_pos_ < write_buf_.size() || file_fd_ >= 0) {
        write_buf_.append(data);
        after_response();
        return;
    }

    const char* ptr = data.data();
    std::size_t rem = data.size();

    while (rem > 0) {
        ssize_t n = conn_.write(ptr, rem);
        if (n == -2) {
            // Fatal error (EPIPE / ECONNRESET / EBADF) — peer or socket
            // is gone. Mark closed and bail; retrying spins forever.
            closed_ = true;
            if (loop_)
                loop_->remove_io(conn_.fd());
            return;
        }
        if (n == -1) {
            // EAGAIN — backpressure. Buffer remainder and register EPOLLOUT.
            if (loop_) {
                write_buf_.assign(ptr, rem);
                write_pos_ = 0;
                after_response();
                return;
            }
            // No EventLoop — legacy short-spin for tests / sync handlers.
            continue;
        }
        if (n == 0) break;
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }

    after_response();
}

void HttpConnection::send_file(const std::string& path, std::string_view mime_type)
{
    if (closed_) return;

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        HttpResponse r;
        r.set_status(HttpStatus::not_found).set_body("file not readable");
        send_response(r);
        return;
    }

    struct stat st;
    if (::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        HttpResponse r;
        r.set_status(HttpStatus::not_found).set_body("not a regular file");
        send_response(r);
        return;
    }

    auto file_size = static_cast<std::size_t>(st.st_size);

    // The answer is going out from here on. The refusals above went through
    // send_response(), which counts them itself.
    response_started();

    // Construct HTTP response headers
    auto headers = fmt::format(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "\r\n",
        mime_type, file_size);

    // If there are already pending writes, append headers and queue file
    if (write_pos_ < write_buf_.size() || file_fd_ >= 0) {
        write_buf_.append(headers);
        // Can't queue two files — fallback to buffered read
        if (file_fd_ >= 0) {
            // Read entire file into write buffer (rare edge case)
            std::string buf(file_size, '\0');
            [[maybe_unused]] ssize_t n = ::read(fd, buf.data(), file_size);
            ::close(fd);
            write_buf_.append(buf);
            after_response();
            return;
        }
        file_fd_ = fd;
        file_offset_ = 0;
        file_remaining_ = file_size;
        after_response();
        return;
    }

    // Try to write headers immediately
    const char* ptr = headers.data();
    std::size_t rem = headers.size();

    while (rem > 0) {
        ssize_t n = conn_.write(ptr, rem);
        if (n == -2) {
            // Fatal — peer gone. Abort send_file; we never queued fd yet.
            ::close(fd);
            closed_ = true;
            if (loop_)
                loop_->remove_io(conn_.fd());
            return;
        }
        if (n == -1) {
            // EAGAIN — buffer headers + queue file
            write_buf_.assign(ptr, rem);
            write_pos_ = 0;
            file_fd_ = fd;
            file_offset_ = 0;
            file_remaining_ = file_size;
            after_response();
            return;
        }
        if (n == 0) { ::close(fd); after_response(); return; }
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }

    // Headers sent — now sendfile
    file_fd_ = fd;
    file_offset_ = 0;
    file_remaining_ = file_size;

    // Fully sent, or partial with EPOLLOUT for the remainder — or, on a
    // failed sendfile, closed (drain_file).
    drain_file();
    after_response();
}

bool HttpConnection::on_writable()
{
    if (closed_) return false;

    // Drain buffered data first (headers, serialized responses)
    if (!drain_buffer())
        return true;

    // Then drain file via sendfile(2)
    if (file_fd_ >= 0) {
        if (!drain_file())
            return true;
    }

    // All out: close if asked, otherwise drop EPOLLOUT and read again.
    after_response();

    return false;
}

bool HttpConnection::has_pending_writes() const noexcept
{
    return write_pos_ < write_buf_.size() || file_fd_ >= 0;
}

bool HttpConnection::drain_buffer()
{
    while (write_pos_ < write_buf_.size()) {
        ssize_t n = conn_.write(
            write_buf_.data() + write_pos_,
            write_buf_.size() - write_pos_);
        if (n == -2) {
            // Fatal — peer gone. Drop the buffered tail; on_writable will
            // observe closed_ and disarm.
            closed_ = true;
            if (loop_)
                loop_->remove_io(conn_.fd());
            write_buf_.clear();
            write_pos_ = 0;
            return true;   // nothing left to drain
        }
        if (n == -1)
            return false;  // EAGAIN — try again on next EPOLLOUT
        if (n == 0) {
            closed_ = true;
            return true;
        }
        write_pos_ += static_cast<std::size_t>(n);
    }
    write_buf_.clear();
    write_pos_ = 0;
    return true;
}

bool HttpConnection::drain_file()
{
    bool fatal = false;
    while (file_remaining_ > 0) {
        ssize_t n = ::sendfile(conn_.fd(), file_fd_,
                               &file_offset_, file_remaining_);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;  // Try again on next EPOLLOUT
            // Actual error (EPIPE, ECONNRESET, EBADF) — peer or socket
            // is gone. Truncated HTTP response is already on the wire;
            // close the connection so the caller tears down instead of
            // leaving it armed for EPOLLIN on a dead fd.
            fatal = true;
            break;
        }
        if (n == 0)
            break;  // EOF
        file_remaining_ -= static_cast<std::size_t>(n);
    }

    ::close(file_fd_);
    file_fd_ = -1;
    file_remaining_ = 0;

    if (fatal) {
        closed_ = true;
        if (loop_)
            loop_->remove_io(conn_.fd());
    }

    return true;
}

void HttpConnection::update_interest()
{
    if (!loop_ || closed_) return;

    uint32_t m = 0;
    if (owed())
        m = EPOLLRDHUP;              // not reading; only watch the peer leave
    else if (!stop_reading_)
        m = EPOLLIN | EPOLLRDHUP;
    if (has_pending_writes() || resumable())
        m |= EPOLLOUT;               // a tail to write, or held bytes to wake for

    // One epoll_ctl per change, not per request: a keep-alive /ping never
    // leaves EPOLLIN | EPOLLRDHUP and never pays for this.
    if (m != mask_) {
        try {
            loop_->modify_io(conn_.fd(), m);
            mask_ = m;
        } catch (const std::system_error&) {
            // The fd is no longer in the loop — dropped by the caller after
            // something threw. Nothing is going to read or write it again;
            // throwing here would do it inside a module's callback.
            closed_ = true;
        }
    }
}

TcpConnection HttpConnection::release_tcp()
{
    if (file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
    closed_ = true;
    return std::move(conn_);
}

} // namespace apostol
