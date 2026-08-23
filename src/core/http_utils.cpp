#include "apostol/http_utils.hpp"
#include "apostol/base64.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <arpa/inet.h>

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

namespace {

// ─── ip_literal ──────────────────────────────────────────────────────────────
//
// Whether the text is an address and nothing else. Callers hand the result of
// get_real_ip to PostgreSQL as inet, and a value that is not an address makes the
// cast throw — losing the whole statement, not just the address, over a header the
// client wrote. inet_pton is deliberately strict: it rejects "1.2.3.4 " and
// "0x01020304", which the older inet_aton accepted.
//
bool ip_literal(const std::string& value)
{
    unsigned char buf[sizeof(struct in6_addr)];
    return !value.empty()
        && (::inet_pton(AF_INET,  value.c_str(), buf) == 1
         || ::inet_pton(AF_INET6, value.c_str(), buf) == 1);
}

// ─── clean_element ───────────────────────────────────────────────────────────
//
// One element of a forwarded-for list, reduced to the address it names. Proxies
// write the port, and bracket an IPv6 literal when they do, because the address is
// all colons itself.
//
std::string clean_element(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return {};
    const auto end = value.find_last_not_of(" \t");
    value = value.substr(begin, end - begin + 1);

    // "[::1]:8080" or "[::1]"
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos)
            return {};
        value = value.substr(1, close - 1);
    } else {
        // "1.2.3.4:80" — a single colon alongside dots is a port, not an address.
        // A bare IPv6 literal has several colons and no dots, and is left alone.
        const auto colon = value.find(':');
        if (colon != std::string_view::npos
            && value.find(':', colon + 1) == std::string_view::npos
            && value.find('.') != std::string_view::npos)
            value = value.substr(0, colon);
    }

    // "fe80::1%eth0" — a scope zone identifies an interface on this host, which
    // means nothing to whoever reads the address later, and PostgreSQL inet does
    // not accept one. The address itself is worth keeping.
    const auto zone = value.find('%');
    if (zone != std::string_view::npos)
        value = value.substr(0, zone);

    return std::string(value);
}

// ─── first_forwarded ─────────────────────────────────────────────────────────
//
// The leftmost address in a forwarded-for list — "client, proxy1, proxy2" (RFC 7239
// §4 describes the same chain for Forwarded). The whole header never was an
// address, so casting it was bound to fail wherever more than one hop existed.
//
// Elements that do not name an address are skipped rather than ending the search:
// RFC 7239 §6.3 allows "unknown" and obfuscated identifiers such as "_hidden" in
// place of one, and a chain that starts with those still carries real addresses
// after them. Skipping widens nothing — whoever could put a forged address behind
// an "unknown" could equally have put it first.
//
std::string first_forwarded(std::string_view value)
{
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto element = clean_element(value.substr(0, comma));

        if (ip_literal(element))
            return element;

        if (comma == std::string_view::npos)
            break;
        value = value.substr(comma + 1);
    }

    return {};
}

} // namespace

// ─── get_real_ip ─────────────────────────────────────────────────────────────

std::string get_real_ip(const HttpRequest& req)
{
    // **Both headers are only as trustworthy as the proxy in front.**
    //
    // A reverse proxy normally writes them, but nothing stops a client from sending
    // them too, and nginx's usual $proxy_add_x_forwarded_for *appends* to whatever
    // arrived — so the leftmost element of X-Forwarded-For is text the client
    // chose. Reading it is right for finding the caller behind a CDN and wrong for
    // deciding anything: it names who the request says it is, not who it is.
    //
    // Deployments that record or gate on this must have the proxy overwrite the
    // header — `proxy_set_header X-Real-IP $remote_addr` does, and X-Real-IP is read
    // first here for that reason. Behind a proxy that only appends, the sole element
    // no client can forge is the rightmost; selecting by hop count is a per-site
    // policy and belongs in configuration rather than here.
    //
    // Each header is used only if it parses as an address; otherwise the peer, which
    // is measured rather than told. The result can still be empty — a peer address
    // is not always available — so a caller passing it to PostgreSQL as inet must
    // send null for an empty string, not ''.
    auto ip = first_forwarded(req.header("X-Real-IP"));
    if (!ip.empty())
        return ip;

    ip = first_forwarded(req.header("X-Forwarded-For"));
    if (!ip.empty())
        return ip;

    return req.peer_ip;
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

// ─── is_private_ip ──────────────────────────────────────────────────────────

bool is_private_ip(std::string_view ip)
{
    if (ip == "::1" || ip.starts_with("127."))
        return true;

    if (ip.starts_with("10.") || ip.starts_with("192.168."))
        return true;

    // 172.16.0.0 – 172.31.255.255
    if (ip.starts_with("172.") && ip.size() > 6) {
        auto dot2 = ip.find('.', 4);
        if (dot2 != std::string_view::npos) {
            int octet = 0;
            for (std::size_t i = 4; i < dot2; ++i) {
                if (ip[i] < '0' || ip[i] > '9') return false;
                octet = octet * 10 + (ip[i] - '0');
            }
            if (octet >= 16 && octet <= 31)
                return true;
        }
    }

    // IPv6 ULA: fc00::/7
    if (ip.starts_with("fc") || ip.starts_with("fd"))
        return true;

    return false;
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
