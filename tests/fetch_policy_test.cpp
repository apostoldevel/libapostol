// fetch_policy_test — FetchAddressPolicy, in this process: the "allow" entries
// it takes, and what a network entry lets through.
//
//   1. a prefix length is digits only, 0..32 for IPv4 and 0..128 for IPv6: a
//      sign, a blank, a suffix or an empty length is refused as a bad prefix;
//      a mapped entry (::ffff:a.b.c.d) takes 96..128, counted in IPv6 bits;
//      an entry with a NUL inside is refused, the error naming it by index and
//      showing the NUL; parse() names the entry;
//   2. the forms it took before are taken as before (names, ports, brackets,
//      leading zeros in a length) and make the same rules;
//   3. a network entry lets through the internal addresses inside it and
//      nothing outside; /0 lets through every internal address of its family;
//      a port rule matches the port of an IPv4 and of an IPv6 peer; a mapped
//      network is the IPv4 network it holds.
//
// Usage: fetch_policy_test

#include "apostol/fetch_policy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("fetch_policy_test: FAIL %s\n", what.c_str());
    }
}

// The socket address of "ip" port "port"; IPv4 or IPv6 by the text.
sockaddr_storage address(const std::string& ip, std::uint16_t port = 80)
{
    sockaddr_storage ss{};
    auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
    if (::inet_pton(AF_INET, ip.c_str(), &s4->sin_addr) == 1) {
        s4->sin_family = AF_INET;
        s4->sin_port   = htons(port);
        return ss;
    }
    auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (::inet_pton(AF_INET6, ip.c_str(), &s6->sin6_addr) == 1) {
        s6->sin6_family = AF_INET6;
        s6->sin6_port   = htons(port);
        return ss;
    }
    std::printf("fetch_policy_test: bad test address '%s'\n", ip.c_str());
    ++g_failed;
    return ss;
}

bool lets_through(const FetchAddressPolicy& p, const std::string& ip, std::uint16_t port = 80,
                  std::string_view host = "example")
{
    const auto ss = address(ip, port);
    return p.check(reinterpret_cast<const sockaddr*>(&ss), host).empty();
}

// parse() with a good entry and "entry": the error it throws, empty if none.
std::string refusal(const std::string& entry)
{
    try {
        (void) FetchAddressPolicy::parse(true, {"10.0.0.1", entry});
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return {};
}

void bad_entries()
{
    const std::vector<std::string> bad_length = {
        "10.0.0.0/-0", "10.0.0.0/-00", "10.0.0.0/-000", "10.0.0.0/+0", "10.0.0.0/-1", "10.0.0.0/-8",
        "10.0.0.0/+8", "::/-0", "::/-00", "fd00::/-0", "[fd00::/-0]", "::/+0", "::1/-128",
        "10.0.0.0/33", "::/129", "10.0.0.0/", "::/", "10.0.0.0/ 8", "10.0.0.0/8 ", "10.0.0.0/8x",
        "10.0.0.0/08x", "10.0.0.0/0x8", "10.0.0.0/8/8", "10.0.0.0/4294967296", "10.0.0.0/4294967328",
    };
    for (const auto& b : bad_length) {
        const auto error = refusal(b);
        check(error.find("'" + b + "'") != std::string::npos && error.find("prefix must be") != std::string::npos,
              "'" + b + "': the error '" + error + "' is not a bad prefix naming the entry");
    }

    for (const std::string b : {"/8", "name/8", "10.0.0.0/8:80", ""}) {
        const auto error = refusal(b);
        check(error.find("'" + b + "'") != std::string::npos,
              "'" + b + "': the error '" + error + "' does not name the entry");
    }

    // A NUL: the error names the entry by its index, shows the whole text with
    // the NUL as "\0" and the offset — what() ends at a raw NUL.
    using namespace std::string_literals;
    struct Nul { std::string entry; std::string shown; std::size_t offset; };
    for (const auto& b : {Nul{"127.0.0.1\0x"s, "127.0.0.1\\0x", 9}, Nul{"10.0.0.0\0x/8"s, "10.0.0.0\\0x/8", 8},
                          Nul{"10.0.0.0/8\0"s, "10.0.0.0/8\\0", 10}, Nul{"backend\0"s, "backend\\0", 7},
                          Nul{"\0::1"s, "\\0::1", 0}, Nul{"a\0b\0"s, "a\\0b\\0", 1}}) {
        const auto error = refusal(b.entry);
        const auto want = "fetch.allow entry 1 '" + b.shown + "': contains a NUL at offset " + std::to_string(b.offset);
        check(error == want, "an entry with a NUL inside: the error '" + error + "', want '" + want + "'");
        check(error.find('\0') == std::string::npos, "a NUL in the error text");
    }
    // ...and counts from 0.
    try {
        (void) FetchAddressPolicy::parse(true, {"x\0"s});
        check(false, "'x\\0' alone: no error");
    } catch (const std::invalid_argument& e) {
        check(std::string(e.what()).rfind("fetch.allow entry 0 '", 0) == 0, std::string("'x\\0' alone: ") + e.what());
    }

    // A mapped entry's length counts in IPv6 bits, 96..128.
    for (const std::string b : {"::ffff:10.0.0.0/8", "::ffff:10.0.0.0/32", "::ffff:10.0.0.0/95", "::ffff:10.0.0.0/0",
                                "::ffff:10.0.0.0/129", "[::ffff:10.0.0.0/-96]", "::ffff:10.0.0.0/+104"}) {
        const auto error = refusal(b);
        check(error.find("'" + b + "'") != std::string::npos && error.find("prefix must be 96..128") != std::string::npos,
              "'" + b + "': the error '" + error + "' is not 'prefix must be 96..128' naming the entry");
    }
}

void good_entries()
{
    const std::vector<std::string> good = {
        "10.0.0.0/0", "10.0.0.0/8", "10.0.0.0/08", "10.0.0.0/32", "10.0.0.5",
        "::/0", "fd00::/7", "::1/128", "[fd00::/8]", "[fd00::1]:443", "::1",
        "backend", "backend:8080", "MinIO", "192.168.0.5:8080", "::ffff:10.0.0.1",
        "::ffff:0:0/96", "::ffff:10.0.0.0/104", "::ffff:10.0.0.1/128", "[::ffff:10.0.0.0/104]",
    };

    for (const auto& g : good) {
        try {
            (void) FetchAddressPolicy::parse(true, {g});
        } catch (const std::invalid_argument& e) {
            check(false, "parse('" + g + "') threw: " + e.what());
        }
    }
}

void matching()
{
    const auto off = FetchAddressPolicy::parse(false, {});
    check(!off.enabled() && lets_through(off, "127.0.0.1"), "policy off refuses");

    const auto none = FetchAddressPolicy::parse(true, {});
    check(none.enabled(), "deny_internal on: not enabled");
    for (const char* ip : {"127.0.0.1", "10.1.2.3", "192.168.1.1", "169.254.169.254", "::1", "fd12::1",
                           "::ffff:127.0.0.1"})
        check(!lets_through(none, ip), std::string("no rules: ") + ip + " let through");
    for (const char* ip : {"8.8.8.8", "2a00:1450::1"})
        check(lets_through(none, ip), std::string("no rules: public ") + ip + " refused");

    const auto p9 = FetchAddressPolicy::parse(true, {"10.0.0.0/9"});
    check(lets_through(p9, "10.0.0.1") && lets_through(p9, "10.127.255.255") && !lets_through(p9, "10.128.0.0")
          && !lets_through(p9, "192.168.1.1") && lets_through(p9, "::ffff:10.1.1.1"), "10.0.0.0/9");
    check(lets_through(FetchAddressPolicy::parse(true, {"10.0.0.0/09"}), "10.127.0.1")
          && !lets_through(FetchAddressPolicy::parse(true, {"10.0.0.0/09"}), "10.128.0.1"), "10.0.0.0/09 = /9");

    const auto p32 = FetchAddressPolicy::parse(true, {"10.0.0.5/32"});
    check(lets_through(p32, "10.0.0.5") && !lets_through(p32, "10.0.0.4"), "/32");

    const auto any4 = FetchAddressPolicy::parse(true, {"0.0.0.0/0"});
    check(lets_through(any4, "127.0.0.1") && lets_through(any4, "169.254.169.254") && !lets_through(any4, "::1")
          && !lets_through(any4, "fd12::1"), "0.0.0.0/0: every internal IPv4, no IPv6");

    const auto v6 = FetchAddressPolicy::parse(true, {"fd00::/8"});
    check(lets_through(v6, "fd12::1") && !lets_through(v6, "fc00::1") && !lets_through(v6, "::1")
          && !lets_through(v6, "10.0.0.1"), "fd00::/8");

    const auto any6 = FetchAddressPolicy::parse(true, {"::/0"});
    check(lets_through(any6, "::1") && lets_through(any6, "fe80::1") && !lets_through(any6, "127.0.0.1")
          && !lets_through(any6, "::ffff:127.0.0.1"), "::/0: every internal IPv6, no IPv4");

    const auto forms = FetchAddressPolicy::parse(true, {"[fd00::/8]", "::1/128", "::ffff:10.0.0.1", "backend"});
    check(lets_through(forms, "fd12::1") && !lets_through(forms, "fc00::1"), "[fd00::/8] = fd00::/8");
    check(lets_through(forms, "::1") && !lets_through(forms, "::2"), "::1/128");
    check(lets_through(forms, "10.0.0.1") && lets_through(forms, "::ffff:10.0.0.1") && !lets_through(forms, "10.0.0.2"),
          "::ffff:10.0.0.1 = 10.0.0.1");
    check(lets_through(forms, "172.17.0.2", 80, "backend") && lets_through(forms, "172.17.0.2", 1, "[BACKEND]")
          && !lets_through(forms, "172.17.0.2", 80, "backend2"), "name: any port, any case");

    // A mapped network is the IPv4 network it holds: /104 = /8, /96 every IPv4, /128 one.
    const auto m104 = FetchAddressPolicy::parse(true, {"::ffff:10.0.0.0/104"});
    check(lets_through(m104, "10.200.0.1") && lets_through(m104, "::ffff:10.1.1.1") && !lets_through(m104, "172.16.0.1")
          && !lets_through(m104, "127.0.0.1") && !lets_through(m104, "::1") && !lets_through(m104, "fd12::1"),
          "::ffff:10.0.0.0/104 = 10.0.0.0/8");
    const auto m96 = FetchAddressPolicy::parse(true, {"::ffff:0:0/96"});
    check(lets_through(m96, "127.0.0.1") && lets_through(m96, "::ffff:169.254.169.254") && !lets_through(m96, "::1")
          && !lets_through(m96, "fd12::1"), "::ffff:0:0/96: every internal IPv4, no IPv6");
    const auto m128 = FetchAddressPolicy::parse(true, {"::ffff:10.0.0.1/128"});
    check(lets_through(m128, "10.0.0.1") && !lets_through(m128, "10.0.0.2"), "::ffff:10.0.0.1/128 = 10.0.0.1");
    const auto m120 = FetchAddressPolicy::parse(true, {"::ffff:10.0.0.0/120"});
    check(lets_through(m120, "10.0.0.255") && !lets_through(m120, "10.0.1.0"), "::ffff:10.0.0.0/120 = 10.0.0.0/24");

    const auto port = FetchAddressPolicy::parse(true, {"10.0.0.5:8080", "backend:9000", "[fd00::1]:443",
                                                       "[::ffff:10.0.0.6]:8443"});
    check(lets_through(port, "fd00::1", 443) && !lets_through(port, "fd00::1", 444) && !lets_through(port, "fd00::2", 443),
          "[fd00::1]:443");
    check(lets_through(port, "10.0.0.6", 8443) && lets_through(port, "::ffff:10.0.0.6", 8443)
          && !lets_through(port, "::ffff:10.0.0.6", 8444), "[::ffff:10.0.0.6]:8443");
    check(lets_through(port, "10.0.0.5", 8080) && !lets_through(port, "10.0.0.5", 8081), "address:port");
    check(lets_through(port, "172.17.0.2", 9000, "Backend") && !lets_through(port, "172.17.0.2", 9001, "backend")
          && !lets_through(port, "172.17.0.2", 9000, "other"), "name:port");

    sockaddr_un un{};
    un.sun_family = AF_UNIX;
    check(!none.check(reinterpret_cast<const sockaddr*>(&un), "x").empty()
          && !any4.check(reinterpret_cast<const sockaddr*>(&un), "x").empty(), "a non-IP address let through");
}

} // namespace

int main()
{
    // An exception out of a section is its FAIL, not the end of the run.
    for (const auto& [name, section] : {std::pair{"bad_entries", bad_entries}, std::pair{"good_entries", good_entries},
                                        std::pair{"matching", matching}}) {
        try {
            section();
        } catch (const std::exception& e) {
            check(false, std::string(name) + " threw: " + e.what());
        }
    }
    std::printf("fetch_policy_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
