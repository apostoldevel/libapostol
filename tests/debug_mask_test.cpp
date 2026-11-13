// debug_mask_test — the masking of the _DEBUG request and response dump
// (debug_mask.hpp), in this process:
//
//   1. a body that is neither JSON nor a form — broken JSON, plain text, a bare
//      token, invalid UTF-8, nesting too deep — is not written: the output
//      holds none of it, only "<masked>" and the size;
//   2. a JSON object or array is masked by name at any depth, the rest is
//      written; a JSON string or number on its own is masked whole;
//   3. a form or a query string is masked by name; a part without '=' or with
//      an empty value is masked whole; a part that is not name=value makes
//      the whole input masked;
//   4. the names: credentials are recognised (old and added names), names
//      that only resemble them (token_type, a numeric "code") are not;
//   5. headers: Authorization keeps its scheme, cookies keep their names, the
//      query and fragment of a URL-valued header are masked by name.
//
// Usage: debug_mask_test

#include "apostol/debug_mask.hpp"

#include <cstdio>
#include <string>
#include <string_view>

using namespace apostol::debug_mask;

namespace
{

int g_failed = 0;

std::string shown(std::string_view s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
        else { char buf[8]; std::snprintf(buf, sizeof buf, "\\x%02x", c); out += buf; }
    }
    return out;
}

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("debug_mask_test: FAIL %s\n", what.c_str());
    }
}

void expect_eq(std::string_view label, const std::string& got, std::string_view expected)
{
    check(got == expected, std::string(label) + ": got \"" + shown(got) + "\", expected \""
                               + shown(expected) + "\"");
}

/// The output carries none of @p secret and says the input was withheld.
void expect_withheld(std::string_view label, const std::string& got, std::string_view secret)
{
    check(got.find(secret) == std::string::npos,
          std::string(label) + ": secret written: \"" + shown(got) + "\"");
    check(got.rfind(masked, 0) == 0 && got.find(" bytes, ") != std::string::npos,
          std::string(label) + ": not withheld: \"" + shown(got) + "\"");
}

// ── 1. Fail closed ───────────────────────────────────────────────────────────

void test_fail_closed()
{
    const std::string secret = "hunter2-s3cr3t";

    expect_withheld("broken JSON", mask_body(R"({"username":"u","password":")" + secret + R"(",)"),
                    secret);
    expect_withheld("JSON cut short", mask_body(R"({"client_secret":")" + secret), secret);
    expect_withheld("plain text", mask_body("my password is " + secret), secret);
    expect_withheld("text with a newline", mask_body("password=" + secret + "\nx=1"), secret);
    expect_withheld("bare JSON string", mask_body("\"" + secret + "\""), secret);
    expect_withheld("bare JSON number", mask_body("4815162342"), "4815162342");
    expect_withheld("invalid UTF-8 in JSON",
                    mask_body("{\"password\":\"" + secret + "\xff\"}"), secret);
    expect_withheld("XML", mask_body("<auth><password>" + secret + "</password></auth>"), secret);

    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "[";
    deep += "\"" + secret + "\"";
    for (int i = 0; i < 100; ++i) deep += "]";
    expect_withheld("nested too deep", mask_body(deep), secret);

    // A bare credential, with '=' padding or without: masked, not printed as a name.
    expect_eq("bare token", mask_body("eyJhbGciOiJIUzI1NiJ9.e30.c2ln"), masked);
    expect_eq("bare base64 with padding", mask_body("aHVudGVyMg=="), masked);
    expect_eq("padding as an empty value", mask_form("aHVudGVyMg="), masked);
    expect_eq("padding among pairs", mask_form("a=1&aHVudGVyMjM==&b=2"), "a=1&<masked>&b=2");

    expect_eq("empty body", mask_body(""), "");
    expect_eq("JSON true", mask_body("true"), "true");
    expect_eq("JSON null", mask_body("null"), "null");
}

// ── 2. JSON ──────────────────────────────────────────────────────────────────

void test_json()
{
    expect_eq("object",
              mask_body(R"({"username":"u","password":"p","scope":"api"})"),
              R"({"password":"<masked>","scope":"api","username":"u"})");
    expect_eq("nested",
              mask_body(R"({"a":{"b":[{"refresh_token":"t","x":1}]}})"),
              R"({"a":{"b":[{"refresh_token":"<masked>","x":1}]}})");
    expect_eq("secret object masked whole",
              mask_body(R"({"credentials":{"user":"u","pass":"p"}})"),
              R"({"credentials":"<masked>"})");
    expect_eq("numeric code is a status",
              mask_body(R"({"error":{"code":404,"message":"Not found"}})"),
              R"({"error":{"code":404,"message":"Not found"}})");
    expect_eq("string code is a credential",
              mask_body(R"({"code":"abc","state":"s"})"),
              R"({"code":"<masked>","state":"s"})");
    expect_eq("numeric pin is masked", mask_body(R"({"pin":1234})"), R"({"pin":"<masked>"})");
    expect_eq("null stays null", mask_body(R"({"password":null})"), R"({"password":null})");
    expect_eq("array", mask_body(R"([{"secret":"s"},2])"), R"([{"secret":"<masked>"},2])");
    expect_eq("strings in an array", mask_body(R"({"method":"login","params":["user","zz9"]})"),
              R"({"method":"login","params":["<masked>","<masked>"]})");
    expect_eq("name/value pair", mask_body(R"({"name":"client_secret","value":"zz9"})"),
              R"({"name":"client_secret","value":"<masked>"})");
    expect_eq("key/value pair", mask_body(R"({"key":"AuthorizationKey","value":"zz9"})"),
              R"({"key":"<masked>","value":"<masked>"})");
    expect_eq("JSON in a string", mask_body(R"({"payload":"{\"password\":\"zz9\"}"})"),
              R"({"payload":"<masked>"})");
    expect_eq("numeric code in a request", mask_body(R"({"phone":"1","code":482913})"),
              R"({"code":"<masked>","phone":"1"})");
    expect_eq("boolean under a secret name", mask_body(R"({"is_private":true})"),
              R"({"is_private":true})");
}

// ── 3. Forms and query strings ───────────────────────────────────────────────

void test_form()
{
    expect_eq("token request",
              mask_form("grant_type=password&username=u&password=p%2Bq&client_id=web"),
              "grant_type=password&username=u&password=<masked>&client_id=web");
    expect_eq("encoded name", mask_form("client%5Fsecret=s&a=1"), "client%5Fsecret=<masked>&a=1");
    expect_eq("bracket name", mask_form("filter[name]=x&user[password]=p"),
              "filter[name]=x&user[password]=<masked>");
    expect_eq("empty value", mask_form("a=&b=1"), "<masked>&b=1");
    expect_eq("bare part", mask_form("a=1&tok3n"), "a=1&<masked>");
    expect_eq("empty parts kept", mask_form("a=1&&b=2"), "a=1&&b=2");
    expect_eq("value holding a query", mask_form("next=/a?b=c&x=1"), "next=<masked>&x=1");
    expect_eq("encoded query in a value", mask_form("next=%2Fcb%3Fcode%3Dzz9&x=1"), "next=<masked>&x=1");
    expect_eq("encoded JSON in a value", mask_form("data=%7B%22password%22%3A%22zz9%22%7D"),
              "data=<masked>");
    expect_eq("pairs after ';'", mask_form("a=1;password=zz9"), "a=<masked>");
    expect_eq("plain value", mask_form("page=2&q=abc"), "page=2&q=abc");
    expect_eq("form body", mask_body("code=abc&state=xyz"), "code=<masked>&state=xyz");

    expect_withheld("brace in a value", mask_form("a={\"password\":\"zz9\"}"), "zz9");
    expect_withheld("blank in a name", mask_form("my password=zz9"), "zz9");
    expect_withheld("quote in a value", mask_form("a=\"zz9\""), "zz9");
    expect_withheld("control byte", mask_form(std::string("a=zz9\x01")), "zz9");
    expect_withheld("non-ASCII", mask_form("a=zz9\xd0\xb9"), "zz9");
    expect_withheld("empty name", mask_form("=zz9"), "zz9");
}

// ── 4. Names ─────────────────────────────────────────────────────────────────

void test_names()
{
    for (std::string_view n : {"password", "Password", "passwd", "new_password", "passphrase",
                               "oldpass", "client_secret", "access_token", "id_token",
                               "x-csrf-token", "credentials", "api_key", "X-Api-Key", "apikey",
                               "private_key", "signature", "x-signature", "cryptogram",
                               "card_cryptogram_packet", "jwt", "bearer", "cookie", "salt",
                               "assertion", "code", "code_verifier", "auth_code", "sms_code",
                               "verification_code", "activation_code", "confirm_code", "ticket",
                               "session", "Session", "Secret", "sid", "pwd", "otp", "pin", "key",
                               "hash", "card_number", "cardnumber", "pan", "cvv", "CVC",
                               "authorization", "proxy-authorization"})
        check(secret_name(n), "secret_name(\"" + std::string(n) + "\") = false");

    for (std::string_view n : {"session_id", "sessionId", "PHPSESSID", "otp_code", "totp_code",
                               "mfa_code", "pin_code", "device_code", "oob_code",
                               "client_assertion", "access_key", "accessKey", "license_key",
                               "encryption_key", "X-Authorization", "X-Auth", "Auth"})
        check(secret_name(n), "secret_name(\"" + std::string(n) + "\") = false");

    for (std::string_view n : {"token_type", "token_type_hint", "username", "scope", "state",
                               "grant_type", "client_id", "redirect_uri", "expires_in",
                               "country_code", "error_code", "keyword", "message", "id"})
        check(!secret_name(n), "secret_name(\"" + std::string(n) + "\") = true");
}

// ── 5. Headers ───────────────────────────────────────────────────────────────

void test_path()
{
    expect_eq("short segments", mask_path("/api/v1/object/42"), "/api/v1/object/42");
    expect_eq("uuid", mask_path("/obj/123e4567-e89b-12d3-a456-426614174000"),
              "/obj/123e4567-e89b-12d3-a456-426614174000");
    expect_eq("token segment", mask_path("/confirm/aHVudGVyMmh1bnRlcjJodW50ZXIyaHVudGVyMg"),
              "/confirm/<masked>");
}

void test_headers()
{
    expect_eq("Authorization", mask_header("Authorization", "Bearer eyJ.x.y"), "Bearer <masked>");
    expect_eq("Authorization without scheme", mask_header("authorization", "abc"), "<masked>");
    expect_eq("Cookie", mask_header("Cookie", "__Host-SID=abc; theme=dark"),
              "__Host-SID=<masked>; theme=<masked>");
    expect_eq("Set-Cookie", mask_header("Set-Cookie", "__Host-SID=abc; Path=/; Secure"),
              "__Host-SID=<masked>; Path=/; Secure");
    expect_eq("Referer query", mask_header("Referer", "https://h/cb?code=abc&state=s"),
              "https://h/cb?code=<masked>&state=s");
    expect_eq("Location fragment",
              mask_header("Location", "https://h/cb#access_token=abc&token_type=Bearer"),
              "https://h/cb#access_token=<masked>&token_type=Bearer");
    expect_eq("Location query and fragment",
              mask_header("Location", "https://h/cb?state=s#refresh_token=r"),
              "https://h/cb?state=s#refresh_token=<masked>");
    expect_eq("Location without query", mask_header("Location", "https://h/a"), "https://h/a");
    expect_eq("secret header", mask_header("X-Api-Key", "k"), "<masked>");
    expect_eq("Session header", mask_header("Session", "s"), "<masked>");
    expect_eq("plain header", mask_header("Content-Type", "application/json"), "application/json");

    for (std::string_view h : {"X-Authorization", "X-Forwarded-Authorization", "X-Access-Key",
                               "Ocp-Apim-Subscription-Key", "Sec-WebSocket-Protocol", "Link",
                               "Refresh", "X-Original-URL", "X-Anything"})
        expect_eq(std::string("unlisted header ") + std::string(h), mask_header(h, "zz9"), masked);
    expect_eq("CORS header", mask_header("Access-Control-Allow-Credentials", "true"), "true");
    expect_eq("Referer userinfo", mask_header("Referer", "https://u:zz9@h/cb?state=s"),
              "https://<masked>@h/cb?state=s");
    expect_eq("Location long path segment",
              mask_header("Location", "https://h/confirm/eyJhbGciOiJIUzI1NiJ9eyJzdWIiOiIxIn0"),
              "https://h/confirm/<masked>");
    expect_eq("nameless cookie", mask_header("Cookie", "dGVzdA=="), masked);
    expect_eq("cookie with empty value", mask_header("Cookie", "zz9="), masked);
    expect_eq("nameless Set-Cookie", mask_header("Set-Cookie", "zz9"), masked);

    const auto broken = mask_header("Referer", "https://h/cb?note=a b&password=zz9");
    check(broken.find("zz9") == std::string::npos, "Referer with a blank: \"" + shown(broken) + "\"");
}

} // namespace

int main()
{
    test_fail_closed();
    test_json();
    test_form();
    test_names();
    test_path();
    test_headers();

    if (g_failed != 0) {
        std::printf("debug_mask_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("debug_mask_test: ok\n");
    return 0;
}
