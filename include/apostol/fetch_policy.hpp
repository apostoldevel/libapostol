#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct sockaddr;

namespace apostol
{

// ─── FetchAddressPolicy ─────────────────────────────────────────────────────
//
// Outbound address filter for CurlClient (T970). Applied at connect time, to
// the address actually dialled — for the first URL, for every redirect target
// and for every address a name resolves to — so neither a redirect nor a DNS
// name pointing inside (127.0.0.1.nip.io, rebinding) gets past it. A check of
// the URL string alone (SQL http.create_request) cannot see either.
//
// Disabled by default. Config:
//
//   "fetch": {
//     "deny_internal": true,
//     "allow": ["ocpp:9220", "minio", "10.1.2.0/24", "192.168.0.5:8080", "[fd00::1]:443"]
//   }
//
// deny_internal refuses loopback, private, link-local (cloud metadata
// 169.254.169.254), CGNAT, multicast, reserved and unspecified addresses, IPv4
// and IPv6, with IPv4 embedded in IPv6 (::ffff:a.b.c.d, 64:ff9b::/96, 2002::/16)
// judged as IPv4. An "allow" entry lets one through:
//   - a name  — matched (case-insensitively, exactly) to the host of the URL being
//               connected, whatever it resolves to: "ocpp" is the container;
//   - an IP or CIDR — matched to the address being dialled;
//   - an optional ":port" (IPv6 in brackets) — then the port must match too.
//
// Applies to CurlClient (every connection, redirects included) and to the
// built-in HttpClient (the resolved address, before connect; no redirects).
//
// Limits — for whoever turns it on:
//   - a proxy (CurlClient::set_proxy, or http_proxy/HTTPS_PROXY/ALL_PROXY in the
//     environment for libcurl) makes the address dialled the proxy's: it needs an
//     allow entry, and then the destination is filtered by the proxy, not here;
//   - the server's own public address is not internal: from a container it may
//     reach ports published on the host — close those at the firewall;
//   - a reload changes the rules for new connections; a keep-alive connection a
//     long-lived CurlClient already holds is reused until it idles out.
//
class FetchAddressPolicy
{
public:
    FetchAddressPolicy() = default;

    /// Throws std::invalid_argument naming the entry that does not parse.
    static FetchAddressPolicy parse(bool deny_internal, const std::vector<std::string>& allow);

    bool enabled() const noexcept { return deny_internal_; }

    /// Empty when the connection may go ahead; otherwise why it may not.
    /// url_host is the host of the URL being connected (brackets allowed).
    std::string check(const struct sockaddr* addr, std::string_view url_host) const;

    /// Loopback, private, link-local, etc. — see above.
    static bool is_internal(const struct sockaddr* addr);

    /// Process-wide policy, set by Application::apply_config() on start and on
    /// reload; CurlClient takes a snapshot of it for each request.
    static void set_process_default(FetchAddressPolicy policy);
    static std::shared_ptr<const FetchAddressPolicy> process_default();

private:
    struct Rule
    {
        std::string               host;     // lower-case name; empty for an address rule
        std::array<std::uint8_t, 16> net{}; // IPv4 in the first 4 bytes
        int                       prefix{-1};
        bool                      v4{false};
        std::uint16_t             port{0};  // 0 = any
    };

    bool                deny_internal_{false};
    std::vector<Rule>   allow_;
};

} // namespace apostol
