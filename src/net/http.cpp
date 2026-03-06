#include "apostol/http.hpp"
#include "apostol/event_loop.hpp"

#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fmt/format.h>
#include <stdexcept>
#include <string>
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

HttpResponse& HttpResponse::set_status(int code, std::string text)
{
    status_code_ = code;
    status_text_ = std::move(text);
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
    // Replace existing header if same name
    for (auto& [k, v] : headers_) {
        if (k == name) { v = std::move(value); return *this; }
    }
    headers_.emplace_back(std::move(name), std::move(value));
    return *this;
}

HttpResponse& HttpResponse::add_header(std::string name, std::string value)
{
    headers_.emplace_back(std::move(name), std::move(value));
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

    // Always include Content-Length
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

    return 0;
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
    if (error_)
        return false;

    llhttp_errno_t err = llhttp_execute(parser_.get(), data, len);
    if (err != HPE_OK) {
        error_    = true;
        error_msg_ = llhttp_errno_name(err);
        return false;
    }
    return true;
}

// ─── HttpConnection ───────────────────────────────────────────────────────────

HttpConnection::HttpConnection(TcpConnection conn, EventLoop* loop)
    : conn_(std::move(conn))
    , loop_(loop)
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

    char buf[8192];
    bool should_close = false;

    // Set the handler so the parser calls it per-request
    parser_.set_handler([&](HttpRequest req) {
        req.peer_ip   = conn_.peer_address();
        req.peer_port = conn_.peer_port();
        req.socket_fd = conn_.fd();
        HttpResponse resp;
        handler(req, resp);
        // Skip response if the handler upgraded to WebSocket (release_tcp() was called)
        // or if the handler marked it as deferred (async PG response)
        if (!closed_ && !resp.is_deferred())
            send_response(resp);
        if (!req.keep_alive())
            should_close = true;
    });

    for (;;) {
        if (closed_) break;   // released for WebSocket upgrade
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

        if (!parser_.feed(buf, static_cast<std::size_t>(n))) {
            closed_ = true;
            return false;
        }
    }

    if (should_close) {
        closed_ = true;
        return false;
    }

    return true;
}

void HttpConnection::send_response(const HttpResponse& resp)
{
    if (closed_) return;

    std::string data = resp.serialize();

    // If there are already pending writes, just append
    if (write_pos_ < write_buf_.size() || file_fd_ >= 0) {
        write_buf_.append(data);
        return;
    }

    const char* ptr = data.data();
    std::size_t rem = data.size();

    while (rem > 0) {
        ssize_t n = conn_.write(ptr, rem);
        if (n < 0) {
            // EAGAIN — buffer remainder and register EPOLLOUT
            if (loop_) {
                write_buf_.assign(ptr, rem);
                write_pos_ = 0;
                update_write_interest();
                return;
            }
            // No EventLoop — spin (legacy behavior for tests)
            continue;
        }
        if (n == 0) break;
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }
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
            ::read(fd, buf.data(), file_size);
            ::close(fd);
            write_buf_.append(buf);
            return;
        }
        file_fd_ = fd;
        file_offset_ = 0;
        file_remaining_ = file_size;
        update_write_interest();
        return;
    }

    // Try to write headers immediately
    const char* ptr = headers.data();
    std::size_t rem = headers.size();

    while (rem > 0) {
        ssize_t n = conn_.write(ptr, rem);
        if (n < 0) {
            // EAGAIN — buffer headers + queue file
            write_buf_.assign(ptr, rem);
            write_pos_ = 0;
            file_fd_ = fd;
            file_offset_ = 0;
            file_remaining_ = file_size;
            update_write_interest();
            return;
        }
        if (n == 0) { ::close(fd); return; }
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }

    // Headers sent — now sendfile
    file_fd_ = fd;
    file_offset_ = 0;
    file_remaining_ = file_size;

    if (drain_file())
        return;  // File fully sent

    // Partial — register EPOLLOUT for remainder
    update_write_interest();
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

    // All done — remove EPOLLOUT interest
    update_write_interest();
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
        if (n < 0)
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
    while (file_remaining_ > 0) {
        ssize_t n = ::sendfile(conn_.fd(), file_fd_,
                               &file_offset_, file_remaining_);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;  // Try again on next EPOLLOUT
            // Actual error (EPIPE, etc.) — give up
            break;
        }
        if (n == 0)
            break;  // EOF
        file_remaining_ -= static_cast<std::size_t>(n);
    }

    ::close(file_fd_);
    file_fd_ = -1;
    file_remaining_ = 0;
    return true;
}

void HttpConnection::update_write_interest()
{
    if (!loop_) return;

    if (has_pending_writes())
        loop_->modify_io(conn_.fd(), EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    else
        loop_->modify_io(conn_.fd(), EPOLLIN | EPOLLRDHUP);
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
