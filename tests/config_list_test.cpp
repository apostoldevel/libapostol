// config_list_test — config_list(), in this process: the forms of a list in
// the configuration.
//
//   1. absent or null — an empty list, nothing rejected, the type is right;
//   2. an array — its strings in order, each taken whole (a comma or a blank
//      inside stays); an element that is not a string is rejected, as JSON;
//   3. one string — split at commas and every ASCII blank, empty pieces
//      dropped (a value read from a file ends with a newline);
//   4. a number, a bool or an object — the wrong type, no items.
//
// Usage: config_list_test

#include "apostol/config.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace apostol;
using nlohmann::json;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("config_list_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

void expect(const ConfigList& l, const std::vector<std::string>& items,
            const std::vector<std::string>& rejected, bool wrong_type, const std::string& what)
{
    check(l.items == items && l.rejected == rejected && l.wrong_type == wrong_type, what);
}

} // namespace

int main()
{
    expect(config_list(nullptr), {}, {}, false, "absent");
    const json null_value = nullptr;
    expect(config_list(&null_value), {}, {}, false, "null");

    const json arr = json::array({"a, b", " c ", "", "d"});
    expect(config_list(&arr), {"a, b", " c ", "", "d"}, {}, false, "array: each element whole, in order");
    const json mixed = json::array({"a", 1, nullptr, json::object({{"k", "v"}}), "b"});
    expect(config_list(&mixed), {"a", "b"}, {"1", "null", R"({"k":"v"})"}, false,
           "array: what is not a string is rejected, as JSON");

    const json str = "a,b c\td\ne\r\vf\fg,, ,h\n";
    expect(config_list(&str), {"a", "b", "c", "d", "e", "f", "g", "h"}, {}, false,
           "string: split at commas and every ASCII blank");
    const json blank = " ,\n";
    expect(config_list(&blank), {}, {}, false, "string: only separators — empty");

    const json num = 5, flag = true, obj = json::object({{"a", "b"}});
    expect(config_list(&num), {}, {}, true, "a number — the wrong type");
    expect(config_list(&flag), {}, {}, true, "a bool — the wrong type");
    expect(config_list(&obj), {}, {}, true, "an object — the wrong type");

    std::printf("config_list_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
