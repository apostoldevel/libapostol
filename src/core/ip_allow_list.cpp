#include "apostol/ip_allow_list.hpp"

#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <arpa/inet.h>

#include <charconv>
#include <cstring>
#include <stdexcept>

namespace apostol
{

namespace
{

// @p mapped — written as ::ffff:a.b.c.d (then @p v4 too).
bool parse_ip(std::string_view text, std::array<std::uint8_t, 16>& out, bool& v4, bool& mapped)
{
    out.fill(0);
    mapped = false;

    // A copy for inet_pton, on the stack: no address is longer, and a NUL
    // inside the text would end it early — the rest would go unread.
    char s[INET6_ADDRSTRLEN + 1];
    if (text.empty() || text.size() >= sizeof s || text.find('\0') != std::string_view::npos)
        return false;
    std::memcpy(s, text.data(), text.size());
    s[text.size()] = '\0';

    // inet_pton is strict: dotted quad only for IPv4, no blanks, no host names.
    in_addr a4{};
    if (::inet_pton(AF_INET, s, &a4) == 1) {
        std::memcpy(out.data(), &a4, 4);
        v4 = true;
        return true;
    }

    in6_addr a6{};
    if (::inet_pton(AF_INET6, s, &a6) != 1)
        return false;

    // ::ffff:a.b.c.d — an IPv4 peer on a dual-stack socket: judged as IPv4.
    static constexpr std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(a6.s6_addr, kMapped, sizeof kMapped) == 0) {
        std::memcpy(out.data(), a6.s6_addr + 12, 4);
        v4     = true;
        mapped = true;
        return true;
    }

    std::memcpy(out.data(), a6.s6_addr, 16);
    v4 = false;
    return true;
}

bool prefix_match(const std::array<std::uint8_t, 16>& a, const std::array<std::uint8_t, 16>& net, int prefix)
{
    int i = 0;
    for (; prefix >= 8; prefix -= 8, ++i)
        if (a[i] != net[i])
            return false;
    if (prefix == 0)
        return true;
    const auto mask = static_cast<std::uint8_t>(0xff << (8 - prefix));
    return (a[i] & mask) == (net[i] & mask);
}

} // namespace

const std::vector<std::string>& IpAllowList::loopback()
{
    static const std::vector<std::string> list = {"127.0.0.0/8", "::1"};
    return list;
}

IpAllowList IpAllowList::parse(const std::vector<std::string>& entries, const std::vector<std::string>& defaults)
{
    IpAllowList list;

    for (const auto& e : entries.empty() ? defaults : entries) {
        const auto slash = e.find('/');
        const std::string_view host = std::string_view(e).substr(0, slash);

        Net  n;
        bool mapped = false;
        if (!parse_ip(host, n.addr, n.v4, mapped))
            throw std::invalid_argument(fmt::format("'{}' is not an IP address or a CIDR", e));

        n.prefix = n.v4 ? 32 : 128;
        if (slash != std::string::npos) {
            // Digits only: no sign (from_chars takes "-0"), no blank, no locale.
            const std::string_view bits = std::string_view(e).substr(slash + 1);
            int p = -1;
            const auto [end, ec] = std::from_chars(bits.data(), bits.data() + bits.size(), p);
            // A mapped entry's length counts in IPv6 bits: ::ffff:10.0.0.0/104 is 10.0.0.0/8.
            const int min = mapped ? 96 : 0;
            const int max = n.v4 && !mapped ? 32 : 128;
            if (bits.empty() || bits.find_first_not_of("0123456789") != std::string_view::npos
                || ec != std::errc{} || end != bits.data() + bits.size() || p < min || p > max)
                throw std::invalid_argument(fmt::format("'{}' has a bad prefix length", e));
            n.prefix = p - min;
        }
        list.nets_.push_back(n);
    }
    return list;
}

IpAllowList IpAllowList::configured(const nlohmann::json* value, std::string_view key,
                                    const std::vector<std::string>& defaults, std::string& error)
{
    error.clear();
    std::vector<std::string> entries;
    try {
        if (value != nullptr && value->is_array()) {
            for (const auto& e : *value)
                if (e.is_string())
                    entries.push_back(e.get<std::string>());
                else
                    throw std::invalid_argument("every entry must be a string");
        } else if (value != nullptr && value->is_string()) {
            std::string token;
            for (char c : value->get<std::string>() + ",") {
                // Any ASCII blank: a value read from a file ends with a newline.
                if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
                    if (!token.empty())
                        entries.push_back(std::move(token));
                    token.clear();
                } else {
                    token += c;
                }
            }
        } else if (value != nullptr && !value->is_null()) {
            throw std::invalid_argument("an array of strings or a string");
        }
        return parse(entries, defaults);
    } catch (const std::exception& e) {
        error = fmt::format("{}: {}", key, e.what());
        return {};
    }
}

bool IpAllowList::allows(std::string_view ip) const
{
    std::array<std::uint8_t, 16> a{};
    bool v4 = false, mapped = false;
    if (!parse_ip(ip, a, v4, mapped))
        return false;
    for (const auto& n : nets_)
        if (n.v4 == v4 && prefix_match(a, n.addr, n.prefix))
            return true;
    return false;
}

} // namespace apostol
