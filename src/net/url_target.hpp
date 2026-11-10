#pragma once

// The target of a client URL — scheme, host, port, path — read one way for
// HttpClient and WsClient (RFC 3986 §3). Internal: not an installed header.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace apostol::detail
{

struct UrlTarget
{
    std::string   host;     // a name or an address; an IPv6 address without brackets
    std::uint16_t port{0};  // 0 — none written: the scheme's default applies
    std::string   path;     // "/" when none; the query kept, the fragment dropped
};

/// The scheme in lower case (RFC 3986 §3.1: case-insensitive).
inline std::string lower_scheme(std::string_view scheme)
{
    std::string s(scheme);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// RFC 3986 §3.1: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
inline bool valid_scheme(std::string_view s)
{
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s.front())))
        return false;
    for (const unsigned char c : s)
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    return true;
}

/// Splits "scheme://rest". A "://" counts only before the first '/', '?' or
/// '#' — one inside a path or a query is not a scheme. None written: `scheme`
/// is left empty and `rest` is the whole URL. False — a malformed scheme.
inline bool split_url_scheme(std::string_view url, std::string& scheme, std::string_view& rest)
{
    scheme.clear();
    rest = url;
    const auto pos = url.find("://");
    if (pos == std::string_view::npos || pos > url.find_first_of("/?#"))
        return true;
    if (!valid_scheme(url.substr(0, pos)))
        return false;
    scheme = lower_scheme(url.substr(0, pos));
    rest   = url.substr(pos + 3);
    return true;
}

/// A port of digits only, 1..65535.
inline bool parse_url_port(std::string_view sv, std::uint16_t& port)
{
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (sv.empty() || ec != std::errc{} || ptr != sv.data() + sv.size() || value == 0 || value > 65535)
        return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

/// Reads "host[:port][/path][?query][#fragment]" — a URL after "scheme://".
/// The authority ends at the first '/', '?' or '#'; the fragment is dropped
/// unread. False — not a target to connect to: an empty host; userinfo
/// ("user@"); an IPv6 address without brackets, or brackets around anything
/// but an IPv6 address; a port that is not 1..65535 in digits; a byte <= 0x20
/// or DEL in the host, the port, the path or the query.
inline bool parse_url_target(std::string_view rest, UrlTarget& out)
{
    out = UrlTarget{};

    if (const auto hash = rest.find('#'); hash != std::string_view::npos)
        rest = rest.substr(0, hash);
    for (const unsigned char c : rest)
        if (c <= 0x20 || c == 0x7f)
            return false;

    const auto end       = rest.find_first_of("/?");
    const auto authority = rest.substr(0, end);
    if (authority.find('@') != std::string_view::npos)
        return false;

    std::string_view host;
    std::string_view port;
    bool             has_port = false;

    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
            return false;
        host = authority.substr(1, close - 1);
        in6_addr addr{};
        if (::inet_pton(AF_INET6, std::string(host).c_str(), &addr) != 1)
            return false;
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return false;
            has_port = true;
            port     = after.substr(1);
        }
    } else {
        const auto colon = authority.find(':');
        host             = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            has_port = true;
            port     = authority.substr(colon + 1);   // a second ':' fails as a port
        }
        if (host.find_first_of("[]") != std::string_view::npos)
            return false;
    }

    if (host.empty())
        return false;
    if (has_port && !parse_url_port(port, out.port))
        return false;

    out.host.assign(host);
    if (end == std::string_view::npos)
        out.path = "/";
    else if (rest[end] == '/')
        out.path.assign(rest.substr(end));
    else
        out.path = "/" + std::string(rest.substr(end));   // "?query" right after the authority
    return true;
}

/// The value of the Host header (RFC 9112 §3.2): an IPv6 address in brackets,
/// the port when it is not the scheme's default.
inline std::string host_header(std::string_view host, std::uint16_t port, std::uint16_t default_port)
{
    std::string out = host.find(':') != std::string_view::npos ? "[" + std::string(host) + "]" : std::string(host);
    if (port != default_port)
        out += ":" + std::to_string(port);
    return out;
}

} // namespace apostol::detail
