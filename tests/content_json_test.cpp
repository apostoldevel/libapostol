// content_json_test — how content_to_json() reads a JSON request body, in this
// process:
//
//   1. nesting up to 128 arrays and objects is parsed as before;
//      one level more gives an empty object, as a body that does not parse;
//   2. a body nested far deeper than that (closed or not, under an object or
//      at the root) gives an empty object, and the result can be written back
//      with dump();
//   3. brackets inside strings are not nesting;
//   4. broken JSON gives an empty object; a form body and an empty body (the
//      query string) are read as before.
//
// Usage: content_json_test

#include "apostol/http_utils.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using namespace apostol;
using json = nlohmann::json;

namespace
{

// The nesting content_to_json() accepts, as http_utils.hpp states it.
constexpr std::size_t k_depth_limit = 128;

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("content_json_test: FAIL %s\n", what.c_str());
    }
}

HttpRequest request(const std::string& content_type, std::string body)
{
    HttpRequest req;
    req.method = "POST";
    req.path = "/";
    req.headers.emplace_back("Content-Type", content_type);
    req.body = std::move(body);
    return req;
}

json from_json_body(std::string body)
{
    return content_to_json(request("application/json; charset=utf-8", std::move(body)));
}

// {"a": [[ … [1] … ]]} — `levels` arrays and objects in all, the root included,
// a number in the innermost one; unclosed, the text stops after the brackets.
std::string nested(std::size_t levels, bool closed = true)
{
    std::string s = "{\"a\":";
    s.append(levels - 1, '[');
    if (!closed)
        return s;
    s += '1';
    s.append(levels - 1, ']');
    s += '}';
    return s;
}

bool empty_object(const json& j)
{
    return j.is_object() && j.empty();
}

void test_limit()
{
    const auto at_limit = nested(k_depth_limit);
    const auto j = from_json_body(at_limit);
    check(j == json::parse(at_limit), "nesting at the limit is parsed");
    check(j.dump() == at_limit, "nesting at the limit is written back as sent");

    check(empty_object(from_json_body(nested(k_depth_limit + 1))),
          "one level over the limit gives an empty object");

    // The innermost container at the limit holding an object with a key.
    const auto object_leaf = "{\"a\":" + std::string(k_depth_limit - 2, '[') + "{\"k\":1}" +
                             std::string(k_depth_limit - 2, ']') + "}";
    check(from_json_body(object_leaf) == json::parse(object_leaf),
          "an object with a key at the limit is parsed");
}

void test_far_too_deep()
{
    constexpr std::size_t k_levels = 100000;

    const auto open_root = from_json_body(std::string(k_levels, '['));
    check(empty_object(open_root), "an unclosed array at the root gives an empty object");
    check(open_root.dump() == "{}", "an unclosed array at the root is written back");

    const auto closed_root = from_json_body(std::string(k_levels, '[') + std::string(k_levels, ']'));
    check(empty_object(closed_root), "a closed array at the root gives an empty object");
    check(closed_root.dump() == "{}", "a closed array at the root is written back");

    const auto under_object = from_json_body(nested(k_levels));
    check(empty_object(under_object), "deep arrays under an object give an empty object");
    check(under_object.dump() == "{}", "deep arrays under an object are written back");

    const auto open_object = from_json_body(nested(k_levels, false));
    check(empty_object(open_object), "unclosed deep arrays under an object give an empty object");
}

void test_brackets_in_strings()
{
    const std::string brackets(k_depth_limit * 4, '[');
    const auto body = "{\"a\":\"" + brackets + "\",\"b\":\"\\\"[[[\"}";
    const auto j = from_json_body(body);
    check(j.is_object() && j.value("a", "") == brackets, "brackets inside a string are text");
    check(j.value("b", "") == "\"[[[", "an escaped quote does not end the string");
}

void test_unchanged()
{
    check(empty_object(from_json_body("{\"a\":")), "broken JSON gives an empty object");
    check(from_json_body("{\"grant_type\":\"password\"}") == json{{"grant_type", "password"}},
          "a flat JSON object is read");
    check(from_json_body("[1,2]") == json::array({1, 2}), "a JSON array at the root is read");
    check(from_json_body("null").is_null(), "null at the root is read");
    check(from_json_body("1") == json(1), "a number at the root is read");
    check(from_json_body("\"x\"") == json("x"), "a string at the root is read");

    const auto form = content_to_json(
        request("application/x-www-form-urlencoded", "a=1&b=%5B%5B"));
    check(form == json{{"a", "1"}, {"b", "[["}}, "a form body is decoded");

    HttpRequest query = request("application/json", "");
    query.params.emplace_back("q", "1");
    check(content_to_json(query) == json{{"q", "1"}}, "an empty body falls back to the query");
}

} // namespace

int main()
{
    test_limit();
    test_far_too_deep();
    test_brackets_in_strings();
    test_unchanged();

    if (g_failed != 0) {
        std::printf("content_json_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("content_json_test: ok\n");
    return 0;
}
