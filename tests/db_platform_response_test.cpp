// db_platform_response_test — the request and response contract of db-platform:
// headers and parameters as JSON, the error envelope, the status of its code.
// The same answers from db_platform::…, from the forwarders of pg_utils.hpp
// and from those of ApostolModule.
//
// Usage: db_platform_response_test

#include "apostol/apostol_module.hpp"
#include "apostol/db_platform_response.hpp"
#include "apostol/pg_utils.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("db_platform_response_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

// The forwarders of ApostolModule are protected statics: reached from a subclass.
struct Forwarders final : ApostolModule
{
    using ApostolModule::check_pg_error;
    using ApostolModule::error_code_to_status;
    using ApostolModule::headers_to_json;
    using ApostolModule::params_to_json;
};

struct Envelope
{
    const char* json;
    int         code;
    const char* message;
    const char* id;
};

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::string>> pairs{{"Host", "a"}, {"X-Q", "\"b\""}};
    const std::string pairs_json = R"({"Host":"a","X-Q":"\"b\""})";
    check(db_platform::headers_to_json(pairs) == pairs_json, "headers_to_json");
    check(db_platform::params_to_json(pairs) == pairs_json, "params_to_json");
    check(db_platform::headers_to_json({}) == "{}", "headers_to_json of none: {}");
    check(apostol::headers_to_json(pairs) == pairs_json && Forwarders::headers_to_json(pairs) == pairs_json,
          "headers_to_json: the forwarders answer the same");
    check(apostol::params_to_json(pairs) == pairs_json && Forwarders::params_to_json(pairs) == pairs_json,
          "params_to_json: the forwarders answer the same");

    const Envelope envelopes[] = {
        {R"({"error":{"code":401,"message":"expired","error":"ERR-401-008"}})", 401, "expired", "ERR-401-008"},
        {R"({"error":{"code":400,"message":"bad","error":"invalid_grant"}})", 400, "bad", ""},
        {R"({"error":{"code":40300,"message":"no"}})", 40300, "no", ""},
        {R"({"result":true})", 0, "", ""},
        {"not json", 0, "", ""},
    };
    for (const auto& e : envelopes) {
        std::string msg, id, msg2, id2, msg3, id3, msg4;
        const int code  = db_platform::check_pg_error(e.json, msg, id);
        const int code2 = apostol::check_pg_error(e.json, msg2, id2);
        const int code3 = Forwarders::check_pg_error(e.json, msg3, id3);
        const int code4 = db_platform::check_pg_error(e.json, msg4);
        check(code == e.code && msg == e.message && id == e.id,
              std::string("check_pg_error: ") + e.json);
        check(code2 == code && msg2 == msg && id2 == id && code3 == code && msg3 == msg && id3 == id
              && code4 == code && msg4 == msg,
              std::string("check_pg_error: the forwarders answer the same: ") + e.json);
    }

    const std::pair<int, HttpStatus> statuses[] = {
        {40100, HttpStatus::unauthorized}, {401, HttpStatus::unauthorized}, {403, HttpStatus::forbidden},
        {404, HttpStatus::not_found},     {409, HttpStatus::conflict},     {503, HttpStatus::service_unavailable},
        {200, HttpStatus::bad_request},   {-1, HttpStatus::bad_request},   {40200, HttpStatus::bad_request},
    };
    for (const auto& [code, status] : statuses) {
        const auto got = db_platform::error_code_to_status(code);
        check(got == status, "error_code_to_status(" + std::to_string(code) + ")");
        check(apostol::error_code_to_status(code) == got && Forwarders::error_code_to_status(code) == got,
              "error_code_to_status(" + std::to_string(code) + "): the forwarders answer the same");
    }

    std::printf("db_platform_response_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
