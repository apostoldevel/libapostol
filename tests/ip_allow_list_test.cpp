// ip_allow_list_test — IpAllowList, in this process: the entries it takes, the
// configuration forms it reads, the peers it admits.
//
//   1. absent, null, an empty array or a blank string — the caller's default;
//      no default — nobody;
//   2. a list replaces the default; one string is split on commas and blanks;
//   3. an entry that does not parse closes the list to everyone, next to good
//      entries too, and the error is "<key>: …" naming it;
//   4. an IPv4-mapped peer is judged as IPv4; a network admits its own family
//      only; prefix lengths cut where they say;
//   5. a peer that does not parse is refused.
//
// Usage: ip_allow_list_test

#include "apostol/ip_allow_list.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("ip_allow_list_test: FAIL %s\n", what.c_str());
    }
}

constexpr std::string_view kKey = "module.x.allow";

IpAllowList configured(const nlohmann::json& value, std::string& error,
                       const std::vector<std::string>& defaults = IpAllowList::loopback())
{
    return IpAllowList::configured(&value, kKey, defaults, error);
}

const std::vector<std::string> kPeers = {
    "127.0.0.1", "127.255.0.3", "::1", "::ffff:127.0.0.1",
    "10.0.0.1", "172.17.0.2", "192.168.1.7", "::ffff:192.168.1.7",
    "8.8.8.8", "::2", "fe80::1", "2001:db8::1",
};

std::vector<std::string> admitted(const IpAllowList& list)
{
    std::vector<std::string> out;
    for (const auto& p : kPeers)
        if (list.allows(p))
            out.push_back(p);
    return out;
}

std::string join(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& e : v)
        s += (s.empty() ? "" : ",") + e;
    return "[" + s + "]";
}

void expect(const IpAllowList& list, const std::vector<std::string>& want, const std::string& what)
{
    const auto got = admitted(list);
    check(got == want, what + ": admitted " + join(got) + ", want " + join(want));
}

const std::vector<std::string> kLoopback = {"127.0.0.1", "127.255.0.3", "::1", "::ffff:127.0.0.1"};

void defaults()
{
    std::string error = "stale";
    expect(IpAllowList::configured(nullptr, kKey, IpAllowList::loopback(), error), kLoopback, "absent");
    check(error.empty(), "absent: error not cleared");

    for (const auto& v : {nlohmann::json(), nlohmann::json::array(), nlohmann::json(""), nlohmann::json(" ,\t, ")}) {
        expect(configured(v, error), kLoopback, "default for " + v.dump());
        check(error.empty(), "default for " + v.dump() + ": error " + error);
    }
    expect(IpAllowList::parse({}, IpAllowList::loopback()), kLoopback, "parse({}, loopback)");

    // No default: nobody — never "everyone".
    expect(IpAllowList::configured(nullptr, kKey, {}, error), {}, "absent, no default");
    check(IpAllowList::parse({}).empty(), "parse({}): not empty");
    check(IpAllowList{}.empty() && admitted(IpAllowList{}).empty(), "IpAllowList{} admits someone");
}

void lists()
{
    std::string error;
    expect(configured(nlohmann::json::array({"192.168.1.0/24", "::1"}), error),
           {"::1", "192.168.1.7", "::ffff:192.168.1.7"}, "array");
    check(error.empty(), "array: error " + error);
    expect(configured(nlohmann::json::array({"10.0.0.1"}), error), {"10.0.0.1"}, "one address");

    for (const char* s : {"192.168.1.0/24,::1", "192.168.1.0/24, ::1", "192.168.1.0/24 ::1",
                          "192.168.1.0/24\t::1", " ,192.168.1.0/24,, ::1 ,", "192.168.1.0/24\n::1\n",
                          "192.168.1.0/24\r\n::1\r\n"}) {
        expect(configured(s, error), {"::1", "192.168.1.7", "::ffff:192.168.1.7"}, std::string("string '") + s + "'");
        check(error.empty(), std::string("string '") + s + "': error " + error);
    }
}

void bad_entries()
{
    const std::vector<std::string> bad = {
        "300.1.1.1", "lo", "localhost", "10.0.0.0/33", "::/129", "10.0.0.0/",
        "10.0.0.0/+8", "10.0.0.0/-1", "10.0.0.0/8x", "10.0.0.0/ 8", "/8", "10.0.0.0/8/8",
        "127.1", "0x7f000001", "10.0.0.0/-0", "10.0.0.0/+0", "::1/-0", "10.0.0.0/08x",
        "::ffff:10.0.0.0/8", "::ffff:10.0.0.0/95", "::ffff:10.0.0.0/129",
    };

    const auto closed = [](const nlohmann::json& v, const std::string& what) {
        std::string error;
        const auto list = configured(v, error);
        check(list.empty() && admitted(list).empty(), what + ": not closed");
        check(error.rfind(std::string(kKey) + ": ", 0) == 0, what + ": error '" + error + "' without the key");
        return error;
    };

    for (const auto& b : bad) {
        check(closed(nlohmann::json::array({b}), "['" + b + "']").find("'" + b + "'") != std::string::npos,
              "['" + b + "']: the error does not name the entry");
        closed(b, "'" + b + "'");
        closed("127.0.0.1, 10.0.0.1, " + b, "good, good, '" + b + "'");
        closed(nlohmann::json::array({"0.0.0.0/0", "::/0", b}), "everyone, '" + b + "'");

        bool threw = false;
        try {
            (void) IpAllowList::parse({"127.0.0.1", b});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "parse with '" + b + "' did not throw");
    }

    for (const auto& j : {nlohmann::json(5), nlohmann::json(true), nlohmann::json::object(),
                          nlohmann::json::array({"127.0.0.1", 5}), nlohmann::json::array({"127.0.0.1", nullptr})})
        closed(j, j.dump());

    // A bad default is the caller's error, and closes the list just the same.
    std::string error;
    check(IpAllowList::configured(nullptr, kKey, {"nope"}, error).empty() && !error.empty(), "bad default: not closed");
}

void matching()
{
    const auto v4 = IpAllowList::parse({"10.0.0.0/8"});
    check(v4.allows("10.1.2.3") && v4.allows("::ffff:10.1.2.3") && !v4.allows("::ffff:11.1.2.3"), "mapped peer");
    check(IpAllowList::parse({"127.0.0.1"}).allows("::ffff:127.0.0.1"), "mapped loopback");
    check(IpAllowList::parse({"::ffff:10.0.0.1"}).allows("10.0.0.1"), "mapped entry");
    const auto m104 = IpAllowList::parse({"::ffff:10.0.0.0/104"});
    check(m104.allows("10.200.0.1") && m104.allows("::ffff:10.1.1.1") && !m104.allows("11.0.0.1"), "mapped /104 = /8");
    const auto m96 = IpAllowList::parse({"::ffff:0:0/96"});
    check(m96.allows("8.8.8.8") && !m96.allows("::1"), "mapped /96 = every IPv4");

    const auto p31 = IpAllowList::parse({"10.0.0.0/31"});
    check(p31.allows("10.0.0.0") && p31.allows("10.0.0.1") && !p31.allows("10.0.0.2"), "/31");
    const auto p25 = IpAllowList::parse({"192.168.1.128/25"});
    check(p25.allows("192.168.1.128") && p25.allows("192.168.1.255") && !p25.allows("192.168.1.127")
          && !p25.allows("192.168.0.200"), "/25");
    const auto p12 = IpAllowList::parse({"172.16.0.0/12"});
    check(p12.allows("172.31.255.255") && !p12.allows("172.32.0.0") && !p12.allows("172.15.255.255"), "/12");
    const auto p32 = IpAllowList::parse({"10.0.0.5/32"});
    check(p32.allows("10.0.0.5") && !p32.allows("10.0.0.4"), "/32");
    const auto v6 = IpAllowList::parse({"fd00::/7", "2001:db8::/33"});
    check(v6.allows("fd12::1") && v6.allows("fc00::1") && !v6.allows("fe00::1") && v6.allows("2001:db8:7fff::1")
          && !v6.allows("2001:db8:8000::1"), "IPv6 prefixes");
    check(IpAllowList::parse({"10.9.9.9/8"}).allows("10.1.1.1"), "host bits beyond the prefix");

    const auto any4 = IpAllowList::parse({"0.0.0.0/0"});
    check(any4.allows("8.8.8.8") && any4.allows("::ffff:8.8.8.8") && !any4.allows("::1") && !any4.allows("2001:db8::1"),
          "IPv4 network, IPv6 peer");
    const auto any6 = IpAllowList::parse({"::/0"});
    check(any6.allows("::1") && any6.allows("2001:db8::1") && !any6.allows("127.0.0.1")
          && !any6.allows("::ffff:127.0.0.1"), "IPv6 network, IPv4 peer");

    const auto all = IpAllowList::parse({"0.0.0.0/0", "::/0"});
    check(all.allows("1.2.3.4"), "everyone");
    for (const char* p : {"", "unknown", "1.2.3", "127.0.0.1 ", "[::1]", "::1%lo", "127.0.0.1:4977"})
        check(!all.allows(p), std::string("peer '") + p + "' admitted");
    check(!all.allows(std::string_view("10.0.0.1\0junk", 13)), "a peer with a NUL inside admitted");
    check(!all.allows(std::string(100, '1')), "a long peer admitted");
}

} // namespace

int main()
{
    defaults();
    lists();
    bad_entries();
    matching();
    std::printf("ip_allow_list_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
