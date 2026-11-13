#include "apostol/apostol_module.hpp"
#include "apostol/http_utils.hpp"
#include "apostol/file_utils.hpp"
#ifdef WITH_POSTGRESQL
#include "apostol/pg_utils.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace apostol
{

// ─── Debug helpers (active only in _DEBUG builds) ─────────────────────────────

#ifdef _DEBUG
// Maximum bytes of body/response written to stderr — mirrors v1 MAX_ERROR_STR.
static constexpr std::size_t k_debug_max = 2048;

/// Write up to k_debug_max bytes of @p s to stderr; append "...\n" if truncated.
static void debug_write(std::string_view s)
{
    if (s.size() <= k_debug_max) {
        ::write(STDERR_FILENO, s.data(), s.size());
    } else {
        ::write(STDERR_FILENO, s.data(), k_debug_max);
        ::write(STDERR_FILENO, "...\n", 4);
    }
}

// The dump below goes to stderr, which a container runtime keeps in its log.
// Credentials are masked in it: header values that carry them, and the values
// of parameters whose names say they are secret, in the query string and in a
// form or JSON body. The names are a heuristic; the rest is written as is.

static constexpr std::string_view k_masked = "<masked>";

static std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A parameter or header name whose value is a credential.
static bool secret_name(std::string_view name)
{
    const auto n = lower(name);
    if (n == "token_type" || n == "token_type_hint")
        return false;
    for (std::string_view part : {"password", "passwd", "secret", "token", "credential",
                                  "api_key", "api-key", "apikey"})
        if (n.find(part) != std::string::npos)
            return true;
    for (std::string_view whole : {"assertion", "code", "code_verifier", "ticket", "session",
                                   "pwd", "otp", "pin", "authorization",
                                   "proxy-authorization", "cookie", "set-cookie"})
        if (n == whole)
            return true;
    return false;
}

/// Bodies nested deeper than this are not parsed for masking: both the
/// masking and the dump recurse.
static constexpr int k_mask_depth = 64;

/// "a=1&password=x" → "a=1&password=<masked>".
static std::string mask_form(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    while (true) {
        auto amp = s.find('&');
        auto part = s.substr(0, amp);
        auto eq = part.find('=');
        if (eq != std::string_view::npos && secret_name(url_decode(part.substr(0, eq)))) {
            out.append(part.substr(0, eq + 1));
            out.append(k_masked);
        } else {
            out.append(part);
        }
        if (amp == std::string_view::npos)
            break;
        out += '&';
        s.remove_prefix(amp + 1);
    }
    return out;
}

static void mask_json(nlohmann::json& j)
{
    if (j.is_object()) {
        for (auto& [k, v] : j.items()) {
            // An error envelope's numeric "code" is a status, not a credential.
            if (secret_name(k) && !v.is_null() && !(k == "code" && v.is_number()))
                v = std::string(k_masked);
            else
                mask_json(v);
        }
    } else if (j.is_array()) {
        for (auto& v : j)
            mask_json(v);
    }
}

/// A JSON body is masked by name at any depth; anything else is read as a form.
static std::string mask_body(std::string_view body)
{
    bool too_deep = false;
    nlohmann::json::parser_callback_t depth_cb =
        [&too_deep](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > k_mask_depth)
                too_deep = true;
            return !too_deep;
        };
    auto j = nlohmann::json::parse(body, depth_cb, /*allow_exceptions=*/false);
    if (too_deep)
        return fmt::format("{} ({} bytes, nested too deep to mask)", k_masked, body.size());
    if (!j.is_discarded() && j.is_structured()) {
        mask_json(j);
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return mask_form(body);
}

/// The value of a header that carries a credential: the scheme of an
/// Authorization header stays, a cookie keeps its name.
static std::string mask_header(std::string_view name, std::string_view value)
{
    const auto n = lower(name);
    if (n == "authorization" || n == "proxy-authorization") {
        auto sp = value.find(' ');
        return sp == std::string_view::npos
            ? std::string(k_masked)
            : fmt::format("{} {}", value.substr(0, sp), k_masked);
    }
    // A URL may carry a code or a token in its query or fragment.
    if (n == "referer" || n == "location" || n == "content-location"
        || n == "x-original-uri" || n == "x-forwarded-uri") {
        auto q = value.find_first_of("?#");
        if (q == std::string_view::npos)
            return std::string(value);
        std::string out(value.substr(0, q + 1));
        auto rest = value.substr(q + 1);
        auto hash = rest.find('#');
        out += mask_form(rest.substr(0, hash));
        if (hash != std::string_view::npos)
            out += '#' + mask_form(rest.substr(hash + 1));
        return out;
    }
    if (n == "cookie") {
        std::string out;
        std::string_view rest = value;
        while (!rest.empty()) {
            auto semi = rest.find(';');
            auto part = rest.substr(0, semi);
            auto eq = part.find('=');
            if (!out.empty())
                out += ';';
            if (eq != std::string_view::npos)
                out.append(part.substr(0, eq + 1));
            out.append(k_masked);
            if (semi == std::string_view::npos)
                break;
            rest.remove_prefix(semi + 1);
        }
        return out;
    }
    if (n == "set-cookie") {
        auto eq = value.find('=');
        if (eq == std::string_view::npos)
            return std::string(k_masked);
        auto semi = value.find(';', eq);
        return fmt::format("{}{}{}", value.substr(0, eq + 1), k_masked,
                           semi == std::string_view::npos ? std::string_view{}
                                                          : value.substr(semi));
    }
    return secret_name(name) ? std::string(k_masked) : std::string(value);
}

/// Mirrors v1 DebugRequest(): dump [ip:port] [fd] + request-line, headers, body.
static void debug_request(const HttpRequest& req)
{
    if (req.query.empty())
        fprintf(stderr, "[%p] [%s:%d] [%d] Request:\n%s %s %s\n",
                static_cast<const void*>(&req),
                req.peer_ip.c_str(), req.peer_port, req.socket_fd,
                req.method.c_str(), req.path.c_str(), req.version.c_str());
    else
        fprintf(stderr, "[%p] [%s:%d] [%d] Request:\n%s %s?%s %s\n",
                static_cast<const void*>(&req),
                req.peer_ip.c_str(), req.peer_port, req.socket_fd,
                req.method.c_str(), req.path.c_str(), mask_form(req.query).c_str(),
                req.version.c_str());

    for (const auto& [k, v] : req.headers)
        fprintf(stderr, "%s: %s\n", k.c_str(), mask_header(k, v).c_str());

    if (!req.body.empty()) {
        const auto body = mask_body(req.body);
        fputc('\n', stderr);
        debug_write(body);
        if (body.size() <= k_debug_max)
            fputc('\n', stderr);
    }

    fputc('\n', stderr);
}

/// Mirrors v1 DebugReply(): dump [ip:port] [fd] + status-line, headers, body.
static void debug_response(const HttpRequest& req, const HttpResponse& resp)
{
    std::string s = resp.serialize();

    // Mask header values and the body as debug_request() does: the head is
    // rebuilt line by line, the body follows the first empty line.
    {
        const auto head_end = s.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            std::string out;
            std::string_view head(s.data(), head_end);
            auto eol = head.find("\r\n");
            out.append(head.substr(0, eol));            // status line
            while (eol != std::string_view::npos) {
                head.remove_prefix(eol + 2);
                eol = head.find("\r\n");
                auto line = head.substr(0, eol);
                auto colon = line.find(':');
                out += "\r\n";
                if (colon == std::string_view::npos) {
                    out.append(line);
                } else {
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ')
                        value.remove_prefix(1);
                    out.append(line.substr(0, colon + 1));
                    out += ' ';
                    out.append(mask_header(line.substr(0, colon), value));
                }
            }
            out += "\r\n\r\n";
            std::string_view body(s.data() + head_end + 4, s.size() - head_end - 4);
            if (!body.empty())
                out.append(mask_body(body));
            s = std::move(out);
        } else {
            s = fmt::format("{} ({} bytes, not masked)\n", k_masked, s.size());
        }
    }

    // Replace CRLF → LF for clean terminal output
    for (std::size_t i = 0; i + 1 < s.size(); ) {
        if (s[i] == '\r' && s[i + 1] == '\n') {
            s.erase(i, 1);
        } else {
            ++i;
        }
    }

    fprintf(stderr, "[%p] [%s:%d] [%d] Response:\n",
            static_cast<const void*>(&resp),
            req.peer_ip.c_str(), req.peer_port, req.socket_fd);
    debug_write(s);
    if (s.size() <= k_debug_max)
        fputc('\n', stderr);
}
#endif // _DEBUG

// ─── Method registration ──────────────────────────────────────────────────────

void ApostolModule::add_method(std::string name, MethodFn fn)
{
    methods_.push_back({std::move(name), std::move(fn)});
    allowed_cache_.clear(); // invalidate lazy cache
}

std::string_view ApostolModule::get_allowed() const
{
    if (allowed_cache_.empty()) {
        for (const auto& entry : methods_) {
            if (!allowed_cache_.empty())
                allowed_cache_ += ", ";
            allowed_cache_ += entry.name;
        }
        allowed_cache_ += allowed_cache_.empty() ? "OPTIONS" : ", OPTIONS";
    }
    return allowed_cache_;
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

bool ApostolModule::check_location(const HttpRequest& /*req*/) const
{
    return true; // default: handle all requests
}

bool ApostolModule::execute(const HttpRequest& req, HttpResponse& resp)
{
    if (!initialized_) {
        init_methods();
        initialized_ = true;
    }

    if (!check_location(req))
        return false; // not our path, let next module handle

#ifdef _DEBUG
    debug_request(req);
#endif

    if (req.method == "OPTIONS") {
        cors(req, resp);         // CORS headers for preflight
        do_options(req, resp);
    } else {
        bool dispatched = false;
        for (auto& entry : methods_) {
            if (entry.name == req.method) {
                cors(req, resp); // CORS headers before handler (mirrors v1 Execute())
                entry.fn(req, resp);
                dispatched = true;
                break;
            }
        }
        if (!dispatched)
            method_not_allowed(req, resp);
    }

#ifdef _DEBUG
    debug_response(req, resp);
#endif

    return true;
}

void ApostolModule::do_options(const HttpRequest& /*req*/, HttpResponse& resp)
{
    resp.set_status(HttpStatus::no_content)
        .set_header("Allow", std::string(get_allowed()));
}

void ApostolModule::method_not_allowed(const HttpRequest& /*req*/, HttpResponse& resp)
{
    resp.set_status(HttpStatus::not_allowed)
        .set_header("Allow", std::string(get_allowed()))
        .set_body("405 Method Not Allowed", "text/plain");
}

// ─── CORS ─────────────────────────────────────────────────────────────────────

void ApostolModule::add_allowed_origin(std::string origin)
{
    allowed_origins_.push_back(std::move(origin));
}

void ApostolModule::load_allowed_origins(const OAuthProviders& providers)
{
    // A reference, not a copy — see the header. The application's providers
    // are declared before its module manager, so they outlive every module.
    origin_providers_ = &providers;
}

void ApostolModule::add_allowed_header(std::string header)
{
    allowed_headers_.push_back(std::move(header));
    allowed_headers_cache_.clear(); // invalidate lazy cache
}

bool ApostolModule::is_origin_allowed(std::string_view origin) const
{
    for (const auto& o : allowed_origins_) {
        if (o == "*" || o == origin)
            return true;
    }
    if (origin_providers_)
        for (const auto& app : origin_providers_->apps())
            for (const auto& o : app.javascript_origins)
                if (o == "*" || o == origin)
                    return true;
    return false;
}

const std::string& ApostolModule::get_allowed_headers_str() const
{
    if (allowed_headers_cache_.empty()) {
        for (const auto& h : allowed_headers_) {
            if (!allowed_headers_cache_.empty())
                allowed_headers_cache_ += ", ";
            allowed_headers_cache_ += h;
        }
    }
    return allowed_headers_cache_;
}

void ApostolModule::cors(const HttpRequest& req, HttpResponse& resp)
{
    auto origin = req.header("Origin");
    if (origin.empty() || !is_origin_allowed(origin))
        return;

    // Echo the actual origin (never "*") so Credentials work (mirrors v1 CORS()).
    // Carried: a handler that defers answers later with a response of its own,
    // and the CORS headers belong on that answer as well.
    resp.set_carried_header("Access-Control-Allow-Origin",      origin)
        .set_carried_header("Access-Control-Allow-Methods",     std::string(get_allowed()))
        .set_carried_header("Access-Control-Allow-Headers",     get_allowed_headers_str())
        .set_carried_header("Access-Control-Allow-Credentials", "true")
        .set_carried_header("Vary",                             "Origin");
}

// ─── HTTP utilities (delegate to free functions in http_utils.hpp) ────────────

std::string ApostolModule::get_real_ip(const HttpRequest& req)
{
    return apostol::get_real_ip(req);
}

std::string ApostolModule::get_origin(const HttpRequest& req)
{
    return apostol::get_origin(req);
}

std::string ApostolModule::get_protocol(const HttpRequest& req)
{
    return apostol::get_protocol(req);
}

std::string ApostolModule::get_host(const HttpRequest& req)
{
    return apostol::get_host(req);
}

void ApostolModule::redirect(HttpResponse& resp,
                              std::string_view location,
                              HttpStatus code)
{
    apostol::redirect(resp, location, code);
}

void ApostolModule::reply_error(HttpResponse& resp,
                                 HttpStatus      status,
                                 std::string_view message)
{
    apostol::reply_error(resp, status, message);
}

void ApostolModule::reply_error(HttpResponse& resp,
                                 int             code,
                                 std::string_view message)
{
    apostol::reply_error(resp, code, message);
}

// ─── Additional HTTP utilities (delegates) ────────────────────────────────────

std::string ApostolModule::get_user_agent(const HttpRequest& req,
                                           std::string_view default_agent)
{
    return apostol::get_user_agent(req, default_agent);
}

nlohmann::json ApostolModule::content_to_json(const HttpRequest& req)
{
    return apostol::content_to_json(req);
}

Authorization ApostolModule::parse_authorization(std::string_view header_value)
{
    return apostol::parse_authorization(header_value);
}

std::string ApostolModule::url_encode(std::string_view s)
{
    return apostol::url_encode(s);
}

std::string ApostolModule::url_decode(std::string_view s)
{
    return apostol::url_decode(s);
}

std::string ApostolModule::json_escape(std::string_view s)
{
    return apostol::json_escape(s);
}

bool ApostolModule::match_path(std::string_view path,
                                const std::vector<std::string>& patterns)
{
    return apostol::match_path(path, patterns);
}

// ─── File serving ─────────────────────────────────────────────────────────────

bool ApostolModule::serve_file(const std::filesystem::path& path,
                                HttpResponse& resp, bool head_only)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return false;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(f)), {});
    const auto  mime = std::string(mime_type(path.extension().string()));

    resp.set_status(HttpStatus::ok).set_body(std::move(content), mime);
    if (head_only)
        resp.suppress_body();

    return true;
}

bool ApostolModule::try_files(const std::filesystem::path& root,
                               const HttpRequest& req,
                               HttpResponse& resp,
                               bool head_only,
                               const std::vector<std::string>& fallbacks)
{
    namespace fs = std::filesystem;

    const auto& path = req.path;

    // Only a path that stays under the root is served; any other is refused
    // before anything is read.
    const auto file_path = path_under_root(root, path);
    if (file_path.empty()) {
        resp.set_status(HttpStatus::bad_request)
            .set_body("400 Bad Request", "text/plain");
        if (head_only)
            resp.suppress_body();
        return true;
    }

    // 1. Try exact file ("/" resolves to the root, a directory: not served here)
    if (serve_file(file_path, resp, head_only))
        return true;

    // 2. Directory handling: try path/index.html
    auto dir_index = file_path / "index.html";

    std::error_code ec;
    if (fs::is_regular_file(dir_index, ec) && !ec) {
        // Redirect to path + "/" if missing trailing slash (fixes relative URLs)
        if (path.back() != '/') {
            resp.set_status(HttpStatus::moved_permanently)
                .set_header("Location", path + "/")
                .set_body("", "text/plain");
            return true;
        }
        // Has trailing slash — serve the index
        if (serve_file(dir_index, resp, head_only))
            return true;
    }

    // 3. Fallbacks (e.g. SPA → /index.html); one not under the root is skipped
    for (const auto& fallback : fallbacks) {
        const auto fallback_path = path_under_root(root, fallback);
        if (fallback_path.empty())
            continue;
        if (serve_file(fallback_path, resp, head_only))
            return true;
    }

    // 4. Nothing found
    resp.set_status(HttpStatus::not_found)
        .set_body("404 Not Found", "text/plain");
    if (head_only)
        resp.suppress_body();
    return true;
}

std::string_view ApostolModule::mime_type(const std::string& ext)
{
    static const std::unordered_map<std::string, std::string_view> types{
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".css",   "text/css"},
        {".js",    "application/javascript"},
        {".mjs",   "application/javascript"},
        {".json",  "application/json"},
        {".xml",   "application/xml"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".webp",  "image/webp"},
        {".svg",   "image/svg+xml"},
        {".ico",   "image/x-icon"},
        {".txt",   "text/plain; charset=utf-8"},
        {".md",    "text/plain; charset=utf-8"},
        {".pdf",   "application/pdf"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
        {".eot",   "application/vnd.ms-fontobject"},
        {".mp4",   "video/mp4"},
        {".webm",  "video/webm"},
        {".gz",    "application/gzip"},
        {".zip",   "application/zip"},
    };

    auto it = types.find(ext);
    return it != types.end() ? it->second : "application/octet-stream";
}

// ─── PostgreSQL utility delegates ─────────────────────────────────────────────

#ifdef WITH_POSTGRESQL

std::string ApostolModule::pq_quote_literal(std::string_view val)
{
    return apostol::pq_quote_literal(val);
}

std::string ApostolModule::headers_to_json(
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    return apostol::headers_to_json(headers);
}

std::string ApostolModule::params_to_json(
    const std::vector<std::pair<std::string, std::string>>& params)
{
    return apostol::params_to_json(params);
}

int ApostolModule::check_pg_error(std::string_view json, std::string& error_message)
{
    return apostol::check_pg_error(json, error_message);
}

int ApostolModule::check_pg_error(std::string_view json, std::string& error_message,
                                  std::string& error_id)
{
    return apostol::check_pg_error(json, error_message, error_id);
}

HttpStatus ApostolModule::error_code_to_status(int error_code)
{
    return apostol::error_code_to_status(error_code);
}

#endif // WITH_POSTGRESQL

} // namespace apostol
