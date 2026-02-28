#include "apostol/http_utils.hpp"
#include "apostol/base64.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace apostol
{

// ─── match_path ──────────────────────────────────────────────────────────────

bool match_path(std::string_view path, const std::vector<std::string>& patterns)
{
    for (const auto& pat : patterns) {
        if (pat.empty())
            continue;

        if (pat.back() == '*') {
            // Trailing '*' glob: "/api/*" matches "/api/anything"
            std::string_view prefix(pat.data(), pat.size() - 1);
            if (path.substr(0, prefix.size()) == prefix)
                return true;
        } else {
            if (path == pat)
                return true;
        }
    }
    return false;
}

// ─── json_escape ────────────────────────────────────────────────────────────

std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// ─── reply_error ─────────────────────────────────────────────────────────────

void reply_error(HttpResponse& resp, HttpStatus status, std::string_view message)
{
    resp.set_status(status)
        .set_body(fmt::format(
            R"({{"error":{{"code":{},"message":"{}"}}}})",
            static_cast<int>(status), json_escape(message)),
            "application/json");
}

void reply_error(HttpResponse& resp, int code, std::string_view message)
{
    resp.set_status(code, std::string(status_text(static_cast<HttpStatus>(code))))
        .set_body(fmt::format(
            R"({{"error":{{"code":{},"message":"{}"}}}})",
            code, json_escape(message)),
            "application/json");
}

// ─── HTTP header utilities ──────────────────────────────────────────────────

std::string get_real_ip(const HttpRequest& req)
{
    return req.header("X-Real-IP");
}

std::string get_origin(const HttpRequest& req)
{
    return req.header("Origin");
}

std::string get_protocol(const HttpRequest& req)
{
    auto proto = req.header("X-Forwarded-Proto");
    return proto.empty() ? "http" : proto;
}

std::string get_host(const HttpRequest& req)
{
    auto host = req.header("Host");
    return host.empty() ? "localhost" : host;
}

std::string get_user_agent(const HttpRequest& req, std::string_view default_agent)
{
    auto ua = req.header("User-Agent");
    return ua.empty() ? std::string(default_agent) : ua;
}

std::string get_hostname()
{
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    return "localhost";
}

void redirect(HttpResponse& resp, std::string_view location, HttpStatus code)
{
    resp.set_status(code)
        .set_header("Location", std::string(location))
        .set_body("", "text/plain");
}

// ─── Authorization parsing ──────────────────────────────────────────────────

Authorization parse_authorization(std::string_view header_value)
{
    Authorization auth;

    // Skip leading whitespace
    auto pos = header_value.find_first_not_of(' ');
    if (pos == std::string_view::npos)
        return auth;
    header_value = header_value.substr(pos);

    if (header_value.size() > 7 &&
        (header_value[0] == 'B' || header_value[0] == 'b') &&
        (header_value[1] == 'e' || header_value[1] == 'E') &&
        (header_value[2] == 'a' || header_value[2] == 'A') &&
        (header_value[3] == 'r' || header_value[3] == 'R') &&
        (header_value[4] == 'e' || header_value[4] == 'E') &&
        (header_value[5] == 'r' || header_value[5] == 'R') &&
        header_value[6] == ' ')
    {
        auth.schema = Authorization::Schema::bearer;
        auth.token = std::string(header_value.substr(7));
    }
    else if (header_value.size() > 6 &&
             (header_value[0] == 'B' || header_value[0] == 'b') &&
             (header_value[1] == 'a' || header_value[1] == 'A') &&
             (header_value[2] == 's' || header_value[2] == 'S') &&
             (header_value[3] == 'i' || header_value[3] == 'I') &&
             (header_value[4] == 'c' || header_value[4] == 'C') &&
             header_value[5] == ' ')
    {
        auth.schema = Authorization::Schema::basic;
        try {
            auto decoded = base64_decode(header_value.substr(6));
            auto colon = decoded.find(':');
            if (colon != std::string::npos) {
                auth.username = decoded.substr(0, colon);
                auth.password = decoded.substr(colon + 1);
            } else {
                auth.username = std::move(decoded);
            }
        } catch (...) {
            // Invalid base64 — leave username/password empty
            auth.schema = Authorization::Schema::none;
        }
    }

    return auth;
}

// ─── URL encoding ──────────────────────────────────────────────────────────

std::string url_encode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

// ─── URL / form decoding ────────────────────────────────────────────────────

static int hex_val(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string url_decode(std::string_view s)
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

nlohmann::json parse_form_body(std::string_view body)
{
    nlohmann::json obj = nlohmann::json::object();
    while (!body.empty()) {
        auto amp = body.find('&');
        auto part = (amp != std::string_view::npos)
                        ? body.substr(0, amp)
                        : body;
        if (!part.empty()) {
            auto eq = part.find('=');
            if (eq != std::string_view::npos)
                obj[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
            else
                obj[url_decode(part)] = "";
        }
        if (amp == std::string_view::npos) break;
        body = body.substr(amp + 1);
    }
    return obj;
}

// ─── content_to_json ────────────────────────────────────────────────────────

nlohmann::json content_to_json(const HttpRequest& req)
{
    if (req.body.empty()) {
        // Fallback: build JSON from URL query params
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, v] : req.params)
            obj[k] = v;
        return obj;
    }

    auto ct = req.content_type();
    // Lowercase for comparison
    std::string ct_lower(ct);
    std::transform(ct_lower.begin(), ct_lower.end(), ct_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ct_lower.find("application/json") != std::string::npos) {
        try {
            return nlohmann::json::parse(req.body);
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    if (ct_lower.find("application/x-www-form-urlencoded") != std::string::npos) {
        return parse_form_body(req.body);
    }

    // Unknown content-type: try form decode
    return parse_form_body(req.body);
}

} // namespace apostol
