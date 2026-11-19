// base64_test — base64_encode() and base64_decode(), in this process.
//
//   1. the RFC 4648 vectors encode and decode; padding is optional on decode;
//   2. whitespace anywhere is skipped, also after the padding;
//   3. a byte outside the alphabet, a third '=', and data after the padding
//      throw std::invalid_argument;
//   4. every length 0..255 of binary data round-trips.
//
// Usage: base64_test

#include "apostol/base64.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("base64_test: FAIL %s\n", what.c_str());
    }
}

bool throws(const std::string& in)
{
    try {
        (void)base64_decode(in);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool decodes_to(const std::string& in, const std::string& want)
{
    try {
        return base64_decode(in) == want;
    } catch (...) {
        return false;
    }
}

} // namespace

int main()
{
    const std::pair<const char*, const char*> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"}, {"A", "QQ=="},
    };
    for (const auto& [plain, enc] : vectors) {
        check(base64_encode(plain) == enc, std::string("encode '") + plain + "'");
        check(decodes_to(enc, plain), std::string("decode '") + enc + "'");
    }

    check(decodes_to("QQ==", "A"), "QQ== is A");
    check(decodes_to("QQ", "A"), "QQ (no padding) is A");
    check(decodes_to("QUI", "AB"), "QUI (no padding) is AB");
    check(decodes_to("QQ== \n", "A"), "whitespace after the padding is skipped");
    check(decodes_to("Q Q\r\n=\t=", "A"), "whitespace inside the data and the padding is skipped");

    check(throws("QQ==garbage!!"), "QQ==garbage!! throws");
    check(throws("QQ==QQ=="), "data after the padding throws");
    check(throws("QQ==="), "a third '=' throws");
    check(throws("QQ=x"), "data after one '=' throws");
    check(throws("QUI=\xff"), "a byte after the padding throws");
    check(throws("Q$Q="), "a byte outside the alphabet throws");
    check(throws("QQ\xff"), "a high byte throws");

    std::string data;
    for (int len = 0; len < 256; ++len) {
        const auto enc = base64_encode(data);
        check(decodes_to(enc, data), "round trip of " + std::to_string(len) + " bytes");
        std::string stripped = enc;
        while (!stripped.empty() && stripped.back() == '=')
            stripped.pop_back();
        check(decodes_to(stripped, data), "round trip of " + std::to_string(len) + " bytes without padding");
        data.push_back(static_cast<char>(len));
    }

    std::printf("base64_test: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
