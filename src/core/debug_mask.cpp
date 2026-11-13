#include "apostol/debug_mask.hpp"
#include "apostol/http_utils.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol::debug_mask
{

namespace
{

// ASCII only: the process may run under a locale (settings "locale") in which
// <cctype> takes bytes >= 0x80 for letters or lowercases 'I' to a non-ASCII byte.

constexpr bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
constexpr bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }
constexpr bool is_hex(unsigned char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool ends_with(std::string_view s, std::string_view tail)
{
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

std::string withheld(std::size_t size, std::string_view why)
{
    return fmt::format("{} ({} bytes, {})", masked, size, why);
}

/// A character of a form field name as clients send it: letters, digits,
/// "_.-", the brackets of "a[b]" names and percent-encoding. Narrower than
/// a value, so that a bare credential ending in '=' padding ("dGVzdA==")
/// is not taken for a name with an empty value.
bool name_char(unsigned char c)
{
    return is_alnum(c) || c == '_' || c == '.' || c == '-' || c == '[' || c == ']' || c == '%';
}

/// A character of a field value as it may come unencoded: the characters
/// RFC 3986 allows in a query (unreserved, percent-encoding, sub-delims but
/// '&', ":@/?"), plus the brackets. Text with any other byte is not a form.
bool value_char(unsigned char c)
{
    if (is_alnum(c))
        return true;
    switch (c) {
        case '-': case '.': case '_': case '~': case '%':
        case '!': case '$': case '\'': case '(': case ')': case '*': case '+':
        case ',': case ';': case '=': case ':': case '@': case '/': case '?':
        case '[': case ']':
            return true;
        default:
            return false;
    }
}

template <typename Pred>
bool all_of(std::string_view s, Pred pred)
{
    return std::all_of(s.begin(), s.end(),
                       [&](char c) { return pred(static_cast<unsigned char>(c)); });
}

/// A value that carries structure of its own once decoded — a URL with a
/// query, pairs, JSON — is masked: what is inside it is not checked by name.
bool nested(std::string_view value)
{
    const auto decoded = url_decode(value);
    return decoded.find_first_of("=&?;{[\"") != std::string::npos;
}

/// Masks by name. A numeric "code" directly under "error" is the status of an
/// error envelope and is kept; strings inside arrays are masked; in a
/// {"name"|"key": <secret name>, "value": …} pair the value is masked; a
/// string holding JSON is masked.
void mask_json(nlohmann::json& j, std::string_view parent = {})
{
    if (j.is_object()) {
        bool pair_secret = false;
        for (const char* label : {"name", "key"}) {
            auto it = j.find(label);
            if (it != j.end() && it->is_string() && secret_name(it->get_ref<const std::string&>()))
                pair_secret = true;
        }
        for (auto& [k, v] : j.items()) {
            const bool status = (k == "code" && v.is_number() && parent == "error");
            if (v.is_null() || v.is_boolean() || status)
                continue;
            if (secret_name(k) || (pair_secret && k == "value"))
                v = std::string(masked);
            else
                mask_json(v, k);
        }
    } else if (j.is_array()) {
        for (auto& v : j) {
            if (v.is_string())
                v = std::string(masked);
            else
                mask_json(v, parent);
        }
    } else if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && (s[first] == '{' || s[first] == '['))
            j = std::string(masked);
    }
}

/// Bodies nested deeper than this are not parsed for masking: both the
/// masking and the dump recurse.
constexpr int k_mask_depth = 64;

/// "8-4-4-4-12" hex: an object identifier, not a credential.
bool is_uuid(std::string_view s)
{
    if (s.size() != 36)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? s[i] != '-' : !is_hex(static_cast<unsigned char>(s[i])))
            return false;
    }
    return true;
}

/// Headers written as they are; every other header value is masked.
bool plain_header(std::string_view n)
{
    if (n.rfind("accept", 0) == 0 || n.rfind("access-control-", 0) == 0
        || n.rfind("if-", 0) == 0 || n.rfind("sec-fetch-", 0) == 0 || n.rfind("sec-ch-", 0) == 0
        || n.rfind("content-", 0) == 0)
        return n != "content-location";
    for (std::string_view h : {"host", "user-agent", "connection", "upgrade", "keep-alive",
                               "cache-control", "pragma", "date", "server", "vary", "etag",
                               "last-modified", "expires", "age", "allow", "origin", "range",
                               "transfer-encoding", "te", "trailer", "via", "dnt",
                               "upgrade-insecure-requests", "x-forwarded-for",
                               "x-forwarded-proto", "x-forwarded-host", "x-forwarded-port",
                               "x-real-ip", "x-request-id", "x-requested-with",
                               "sec-websocket-version", "sec-websocket-key",
                               "sec-websocket-accept", "sec-websocket-extensions",
                               "x-content-type-options", "x-frame-options",
                               "strict-transport-security", "referrer-policy",
                               "x-xss-protection", "retry-after", "www-authenticate"})
        if (n == h)
            return true;
    return false;
}

/// A URL: userinfo masked, the path through mask_path, the query and the
/// fragment through mask_form.
std::string mask_url(std::string_view value)
{
    std::string out;
    std::string_view rest = value;
    if (const auto scheme = rest.find("://"); scheme != std::string_view::npos) {
        out.append(rest.substr(0, scheme + 3));
        rest.remove_prefix(scheme + 3);
        const auto end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, end);
        if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
            out.append(masked).append("@");
            authority.remove_prefix(at + 1);
        }
        out.append(authority);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
    }
    const auto q = rest.find_first_of("?#");
    out += mask_path(rest.substr(0, q));
    if (q == std::string_view::npos)
        return out;
    out += rest[q];
    if (rest[q] == '#')
        return out + mask_form(rest.substr(q + 1));
    rest.remove_prefix(q + 1);
    const auto hash = rest.find('#');
    out += mask_form(rest.substr(0, hash));
    if (hash != std::string_view::npos)
        out += '#' + mask_form(rest.substr(hash + 1));
    return out;
}

} // namespace

bool secret_name(std::string_view name)
{
    const auto n = lower(name);
    if (n == "token_type" || n == "token_type_hint")
        return false;

    // Substrings. "pass" covers password, passwd, passphrase, passcode;
    // "auth" covers authorization, x-auth, auth_code. Some names that hold no
    // secret match as well (passenger, author) — a dump can spare them.
    for (std::string_view part : {"pass", "secret", "token", "credential", "auth", "session",
                                  "sessid", "assertion", "verifier", "otp", "api_key", "api-key", "apikey",
                                  "private", "signature", "cryptogram", "jwt", "bearer",
                                  "cookie", "salt", "cvv", "cvc", "card_number", "cardnumber"})
        if (n.find(part) != std::string::npos)
            return true;

    // Endings: access_key, licenseKey, ticket, sid, PIN codes.
    for (std::string_view tail : {"key", "ticket", "sid", "pin", "pwd", "hash"})
        if (ends_with(n, tail))
            return true;

    // "...code" is a credential (code, sms_code, device_code) unless it names
    // a classifier.
    if (ends_with(n, "code")) {
        for (std::string_view benign : {"country", "currency", "error", "status", "postal",
                                        "zip", "lang", "language", "locale", "region",
                                        "response", "result", "http", "reason", "type", "unit"})
            if (n.find(benign) != std::string::npos)
                return false;
        return true;
    }

    return n == "pan";
}

std::string mask_form(std::string_view s)
{
    const auto size = s.size();
    std::string out;
    out.reserve(size);
    while (true) {
        const auto amp = s.find('&');
        const auto part = s.substr(0, amp);
        const auto eq = part.find('=');

        if (!part.empty()) {
            if (eq == std::string_view::npos) {
                // A bare value: the whole part may be the credential.
                if (!all_of(part, value_char))
                    return withheld(size, "not a form");
                out.append(masked);
            } else {
                const auto name = part.substr(0, eq);
                const auto value = part.substr(eq + 1);
                if (name.empty() || !all_of(name, name_char) || !all_of(value, value_char))
                    return withheld(size, "not a form");
                if (value.empty() || value.front() == '=')
                    out.append(masked);    // "<credential>=", "<credential>==": padding, not a pair
                else if (secret_name(url_decode(name)) || nested(value))
                    out.append(name).append("=").append(masked);
                else
                    out.append(part);
            }
        }

        if (amp == std::string_view::npos)
            break;
        out += '&';
        s.remove_prefix(amp + 1);
    }
    return out;
}

std::string mask_path(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    while (true) {
        const auto slash = path.find('/');
        const auto segment = path.substr(0, slash);
        if (segment.size() >= 32 && !is_uuid(segment))
            out.append(masked);
        else
            out.append(segment);
        if (slash == std::string_view::npos)
            break;
        out += '/';
        path.remove_prefix(slash + 1);
    }
    return out;
}

std::string mask_body(std::string_view body)
{
    if (body.empty())
        return {};

    bool too_deep = false;
    nlohmann::json::parser_callback_t depth_cb =
        [&too_deep](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > k_mask_depth)
                too_deep = true;
            return !too_deep;
        };
    auto j = nlohmann::json::parse(body, depth_cb, /*allow_exceptions=*/false);
    if (too_deep)
        return withheld(body.size(), "nested too deep to mask");

    if (!j.is_discarded()) {
        if (j.is_structured()) {
            mask_json(j);
            return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
        // A token or a code may be the whole body.
        if (j.is_string() || j.is_number())
            return withheld(body.size(), "a JSON scalar");
        return std::string(body);    // true, false, null
    }

    return mask_form(body);
}

std::string mask_header(std::string_view name, std::string_view value)
{
    const auto n = lower(name);
    if (n == "authorization" || n == "proxy-authorization") {
        const auto sp = value.find(' ');
        return sp == std::string_view::npos
            ? std::string(masked)
            : fmt::format("{} {}", value.substr(0, sp), masked);
    }
    if (n == "referer" || n == "location" || n == "content-location")
        return mask_url(value);
    if (n == "cookie") {
        std::string out;
        std::string_view rest = value;
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            auto part = rest.substr(0, semi);
            if (!out.empty())
                out += ';';
            while (!part.empty() && part.front() == ' ') {
                out += ' ';
                part.remove_prefix(1);
            }
            const auto eq = part.find('=');
            // The name is kept only when it reads as a name and a value follows.
            if (eq != std::string_view::npos && eq > 0 && eq + 1 < part.size()
                && part[eq + 1] != '=' && all_of(part.substr(0, eq), name_char))
                out.append(part.substr(0, eq + 1));
            out.append(masked);
            if (semi == std::string_view::npos)
                break;
            rest.remove_prefix(semi + 1);
        }
        return out;
    }
    if (n == "set-cookie") {
        const auto eq = value.find('=');
        if (eq == std::string_view::npos || eq == 0 || !all_of(value.substr(0, eq), name_char))
            return std::string(masked);
        const auto semi = value.find(';', eq);
        return fmt::format("{}{}{}", value.substr(0, eq + 1), masked,
                           semi == std::string_view::npos ? std::string_view{}
                                                          : value.substr(semi));
    }
    return plain_header(n) ? std::string(value) : std::string(masked);
}

} // namespace apostol::debug_mask
