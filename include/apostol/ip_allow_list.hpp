#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── IpAllowList ────────────────────────────────────────────────────────────
//
// Inbound address gate: an endpoint answers only the peers whose address falls
// into one of the networks listed. Nothing passes but what is listed.
//
// An entry is an IP address or a network in CIDR form, IPv4 or IPv6:
// "10.0.0.5", "192.168.1.0/24", "::1", "fd00::/8". A peer written as an
// IPv4-mapped IPv6 address (::ffff:a.b.c.d, a dual-stack socket) is judged as
// IPv4, and so is such an entry — its prefix length counted in IPv6 bits, 96 to
// 128 (::ffff:10.0.0.0/104 is 10.0.0.0/8). An IPv4 network never admits an IPv6
// peer, nor the other way round. Host names are not entries; a prefix length is
// digits only.
//
// From the configuration (configured()): an array of strings, or one string
// split on commas and blanks (newlines included), so that the value can come
// from the environment ("allow": "${SOME_ALLOW}"). Absent, null, an empty array or a blank string —
// the caller's default list. An entry that does not parse closes the endpoint
// to everyone, never opens it and never drops just that entry: the list
// returned admits nobody and the error names the entry.
//
//   std::string error;
//   const nlohmann::json* value = cfg.contains("allow") ? &cfg["allow"] : nullptr;
//   auto allow = IpAllowList::configured(value, "module.X.allow",
//                                        IpAllowList::loopback(), error);
//   if (!error.empty()) log.error("{} — closed to everyone", error);
//   ...
//   if (!allow.allows(req.peer_ip)) → 403
//
class IpAllowList
{
public:
    /// Admits nobody.
    IpAllowList() = default;

    /// 127.0.0.0/8 and ::1 — a default for an endpoint meant for the host only.
    static const std::vector<std::string>& loopback();

    /// @p entries, or @p defaults when @p entries is empty (both empty — admits
    /// nobody). Throws std::invalid_argument naming the entry that does not parse.
    static IpAllowList parse(const std::vector<std::string>& entries,
                             const std::vector<std::string>& defaults = {});

    /// The configuration's value (@p value null when the key is absent): see
    /// above. Never throws: what does not parse gives a list that admits
    /// nobody, and @p error — "<key>: <why>" — says why; otherwise @p error is
    /// cleared.
    static IpAllowList configured(const nlohmann::json* value, std::string_view key,
                                  const std::vector<std::string>& defaults, std::string& error);

    /// @p ip as HttpRequest::peer_ip gives it. What does not parse — refused.
    bool allows(std::string_view ip) const;

    /// Admits nobody.
    bool empty() const noexcept { return nets_.empty(); }

private:
    struct Net
    {
        std::array<std::uint8_t, 16> addr{};   // IPv4 in the first 4 bytes
        int                          prefix{0};
        bool                         v4{false};
    };

    std::vector<Net> nets_;
};

} // namespace apostol
