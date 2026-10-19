#include "apostol/fetch_policy.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace apostol
{

namespace
{

// An address as dialled: IPv4 in the first 4 bytes, ::ffff:a.b.c.d already
// folded to IPv4 — it is the same peer.
struct Addr
{
    bool                         v4{false};
    std::array<std::uint8_t, 16> b{};
    std::uint16_t                port{0};
};

bool to_addr(const struct sockaddr* sa, Addr& out)
{
    if (!sa)
        return false;

    if (sa->sa_family == AF_INET) {
        const auto* s = reinterpret_cast<const sockaddr_in*>(sa);
        out.v4 = true;
        std::memcpy(out.b.data(), &s->sin_addr, 4);
        out.port = ntohs(s->sin_port);
        return true;
    }

    if (sa->sa_family == AF_INET6) {
        const auto* s = reinterpret_cast<const sockaddr_in6*>(sa);
        const auto* a = s->sin6_addr.s6_addr;
        out.port = ntohs(s->sin6_port);
        static constexpr std::uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (std::memcmp(a, mapped, 12) == 0) {
            out.v4 = true;
            std::memcpy(out.b.data(), a + 12, 4);
        } else {
            out.v4 = false;
            std::memcpy(out.b.data(), a, 16);
        }
        return true;
    }

    return false;
}

bool prefix_match(const std::uint8_t* a, const std::uint8_t* net, int prefix)
{
    int full = prefix / 8;
    if (std::memcmp(a, net, static_cast<std::size_t>(full)) != 0)
        return false;
    int rest = prefix % 8;
    if (rest == 0)
        return true;
    auto mask = static_cast<std::uint8_t>(0xff << (8 - rest));
    return (a[full] & mask) == (net[full] & mask);
}

bool internal_v4(const std::uint8_t* a)
{
    struct Net { std::uint8_t b[4]; int prefix; };
    static constexpr Net nets[] = {
        {{0, 0, 0, 0},       8},   // "this" network, 0.0.0.0
        {{10, 0, 0, 0},      8},   // private
        {{100, 64, 0, 0},   10},   // CGNAT
        {{127, 0, 0, 0},     8},   // loopback
        {{169, 254, 0, 0},  16},   // link-local — cloud metadata 169.254.169.254
        {{172, 16, 0, 0},   12},   // private — docker bridges
        {{192, 0, 0, 0},    24},   // IETF protocol assignments
        {{192, 0, 2, 0},    24},   // TEST-NET-1
        {{192, 88, 99, 0},  24},   // 6to4 relay anycast
        {{192, 168, 0, 0},  16},   // private
        {{198, 18, 0, 0},   15},   // benchmarking
        {{198, 51, 100, 0}, 24},   // TEST-NET-2
        {{203, 0, 113, 0},  24},   // TEST-NET-3
        {{224, 0, 0, 0},     4},   // multicast
        {{240, 0, 0, 0},     4},   // reserved, broadcast
    };
    return std::any_of(std::begin(nets), std::end(nets),
        [a](const Net& n) { return prefix_match(a, n.b, n.prefix); });
}

bool internal_v6(const std::uint8_t* a)
{
    // IPv4 inside IPv6, judged as IPv4: NAT64 64:ff9b::/96, 6to4 2002::/16.
    static constexpr std::uint8_t nat64[12] = {0x00,0x64,0xff,0x9b,0,0,0,0,0,0,0,0};
    if (std::memcmp(a, nat64, 12) == 0)
        return internal_v4(a + 12);
    if (a[0] == 0x20 && a[1] == 0x02)
        return internal_v4(a + 2);

    struct Net { std::uint8_t b[16]; int prefix; };
    static constexpr Net nets[] = {
        {{0x00}, 8},                                   // ::/8 — ::, ::1, IPv4-compatible
        {{0x01, 0x00}, 64},                            // discard-only 100::/64
        {{0x20, 0x01, 0x0d, 0xb8}, 32},                // documentation
        {{0xfc}, 7},                                   // unique local
        {{0xfe, 0x80}, 10},                            // link-local
        {{0xfe, 0xc0}, 10},                            // site-local (deprecated)
        {{0xff}, 8},                                   // multicast
    };
    return std::any_of(std::begin(nets), std::end(nets),
        [a](const Net& n) { return prefix_match(a, n.b, n.prefix); });
}

std::string to_text(const Addr& a)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (a.v4) {
        ::inet_ntop(AF_INET, a.b.data(), buf, sizeof buf);
        return fmt::format("{}:{}", buf, a.port);
    }
    ::inet_ntop(AF_INET6, a.b.data(), buf, sizeof buf);
    return fmt::format("[{}]:{}", buf, a.port);
}

std::string lower(std::string_view s)
{
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return r;
}

std::string_view strip_brackets(std::string_view h)
{
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']')
        return h.substr(1, h.size() - 2);
    return h;
}

std::mutex                                 g_mutex;
std::shared_ptr<const FetchAddressPolicy>  g_default = std::make_shared<FetchAddressPolicy>();

} // namespace

// ─── parse ──────────────────────────────────────────────────────────────────

FetchAddressPolicy FetchAddressPolicy::parse(bool deny_internal, const std::vector<std::string>& allow)
{
    FetchAddressPolicy p;
    p.deny_internal_ = deny_internal;

    for (const auto& entry : allow) {
        auto bad = [&entry](std::string_view why) {
            return std::invalid_argument(fmt::format("fetch.allow entry '{}': {}", entry, why));
        };

        std::string_view e(entry);
        if (e.empty())
            throw bad("empty");

        Rule r;
        std::string_view host = e;
        std::string_view port;
        bool has_port = false;

        if (e.front() == '[') {
            auto close = e.find(']');
            if (close == std::string_view::npos)
                throw bad("no closing ']'");
            host = e.substr(1, close - 1);
            auto tail = e.substr(close + 1);
            if (!tail.empty()) {
                if (tail.front() != ':')
                    throw bad("expected ':port' after ']'");
                port = tail.substr(1);
                has_port = true;
            }
        } else if (std::count(e.begin(), e.end(), ':') == 1) {
            auto colon = e.find(':');
            host = e.substr(0, colon);
            port = e.substr(colon + 1);
            has_port = true;
        }

        if (has_port) {
            unsigned v = 0;
            auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), v);
            if (port.empty() || ec != std::errc{} || ptr != port.data() + port.size() || v == 0 || v > 65535)
                throw bad("port must be 1..65535");
            r.port = static_cast<std::uint16_t>(v);
        }

        std::string h(host);
        auto slash = h.find('/');
        std::string ip = slash == std::string::npos ? h : h.substr(0, slash);

        std::uint8_t buf[16] = {};
        if (::inet_pton(AF_INET, ip.c_str(), buf) == 1) {
            r.v4 = true;
            std::memcpy(r.net.data(), buf, 4);
            r.prefix = 32;
        } else if (::inet_pton(AF_INET6, ip.c_str(), buf) == 1) {
            static constexpr std::uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
            if (std::memcmp(buf, mapped, 12) == 0) {
                r.v4 = true;
                std::memcpy(r.net.data(), buf + 12, 4);
                r.prefix = 32;
            } else {
                std::memcpy(r.net.data(), buf, 16);
                r.prefix = 128;
            }
        } else if (slash != std::string::npos) {
            throw bad("not an IP network");
        } else {
            if (h.empty())
                throw bad("empty host");
            if (e.front() == '[')
                throw bad("brackets are for an IPv6 address");
            for (unsigned char c : h)
                if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_'))
                    throw bad("not a host name, IP or CIDR");
            // "127.1", "0x7f.1", "2130706433": an IPv4 address in a form other than
            // dotted quad. libcurl reports it as 127.0.0.1 (never matches a name
            // rule), getaddrinfo dials it as 127.0.0.1 (matches) — one rule, two
            // meanings. Written as a.b.c.d it is an address rule.
            if (in_addr tmp{}; ::inet_aton(h.c_str(), &tmp) != 0)
                throw bad("an IPv4 address must be written as a.b.c.d");
            r.host = lower(h);
        }

        if (slash != std::string::npos) {
            if (r.port != 0)
                throw bad("a network takes no port");
            auto len = std::string_view(h).substr(slash + 1);
            int v = -1;
            auto [ptr, ec] = std::from_chars(len.data(), len.data() + len.size(), v);
            int max = r.v4 ? 32 : 128;
            if (len.empty() || ec != std::errc{} || ptr != len.data() + len.size() || v < 0 || v > max)
                throw bad(fmt::format("prefix must be 0..{}", max));
            r.prefix = v;
        }

        p.allow_.push_back(std::move(r));
    }

    return p;
}

// ─── check ──────────────────────────────────────────────────────────────────

bool FetchAddressPolicy::is_internal(const struct sockaddr* sa)
{
    Addr a;
    if (!to_addr(sa, a))
        return true;    // not IP — nothing an outbound HTTP fetch should dial
    return a.v4 ? internal_v4(a.b.data()) : internal_v6(a.b.data());
}

std::string FetchAddressPolicy::check(const struct sockaddr* sa, std::string_view url_host) const
{
    if (!deny_internal_)
        return {};

    Addr a;
    if (!to_addr(sa, a))
        return fmt::format("'{}': not an IP address; fetch.deny_internal refuses it", url_host);

    if (!(a.v4 ? internal_v4(a.b.data()) : internal_v6(a.b.data())))
        return {};

    const auto host = lower(strip_brackets(url_host));

    for (const auto& r : allow_) {
        if (r.port != 0 && r.port != a.port)
            continue;
        if (!r.host.empty()) {
            if (r.host == host)
                return {};
        } else if (r.v4 == a.v4 && prefix_match(a.b.data(), r.net.data(), r.prefix)) {
            return {};
        }
    }

    return fmt::format("'{}' -> {}: internal address, refused by fetch.deny_internal "
                       "(not in fetch.allow)", url_host, to_text(a));
}

// ─── process default ────────────────────────────────────────────────────────

void FetchAddressPolicy::set_process_default(FetchAddressPolicy policy)
{
    auto p = std::make_shared<const FetchAddressPolicy>(std::move(policy));
    std::lock_guard lock(g_mutex);
    g_default = std::move(p);
}

std::shared_ptr<const FetchAddressPolicy> FetchAddressPolicy::process_default()
{
    std::lock_guard lock(g_mutex);
    return g_default;
}

} // namespace apostol
