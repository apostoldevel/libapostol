#ifdef WITH_POSTGRESQL

#include "apostol/db_platform_response.hpp"

#include <nlohmann/json.hpp>

namespace apostol::db_platform
{

// ─── headers_to_json ────────────────────────────────────────────────────────

std::string headers_to_json(
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [k, v] : headers)
        obj[k] = v;
    return obj.dump();
}

// ─── params_to_json ─────────────────────────────────────────────────────────

std::string params_to_json(
    const std::vector<std::pair<std::string, std::string>>& params)
{
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [k, v] : params)
        obj[k] = v;
    return obj.dump();
}

// ─── check_pg_error ─────────────────────────────────────────────────────────

int check_pg_error(std::string_view json, std::string& error_message)
{
    std::string ignored;
    return check_pg_error(json, error_message, ignored);
}

// ─── check_pg_error (with identifier) ───────────────────────────────────────

int check_pg_error(std::string_view json, std::string& error_message,
                   std::string& error_id)
{
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.contains("error"))
            return 0;

        auto& err = j["error"];
        int code = 0;
        if (err.contains("code") && err["code"].is_number())
            code = err["code"].get<int>();
        if (err.contains("message") && err["message"].is_string())
            error_message = err["message"].get<std::string>();

        // Only what looks like an identifier. The same field carries an RFC 6749
        // §5.2 error code in the OAuth 2.0 responses — "invalid_grant",
        // "unauthorized_client" — and handing one of those back as an identifier
        // would have callers matching on it as if it named a catalogue entry.
        if (err.contains("error") && err["error"].is_string()) {
            auto value = err["error"].get<std::string>();
            if (value.rfind("ERR-", 0) == 0)
                error_id = std::move(value);
        }

        return code;
    } catch (...) {
        return 0;
    }
}

// ─── error_code_to_status ───────────────────────────────────────────────────

HttpStatus error_code_to_status(int error_code)
{
    if (error_code >= 10000)
        error_code /= 100;

    // The code is an HTTP status wherever it was written as one: db-platform's
    // "ERR-GGG-CCC" puts the status group in GGG, and the layers above write
    // the status out by hand in their error envelopes. Such a code is relayed
    // as it stands, so the refusal a caller reads on the status line is the one
    // the database declared. The status line is what a foreign integrator, a
    // proxy and a retry policy read; a client reading the body never saw the
    // difference, which is why this went unnoticed for as long as it did.
    //
    // Refusals only. A 2xx or a 3xx inside an "error" envelope is not a
    // refusal this can honestly relay — it is a layer using the envelope for
    // something else (rest.api answers /ping that way) — so it keeps
    // bad_request rather than turning an error into a success on the wire. So
    // does a code that is no HTTP status at all, and the -1 db-platform
    // reports for a message it could not parse.
    //
    // WHERE THE PREMISE FAILS, said out loud because nothing but the enum
    // below is holding it shut. In the legacy five-digit form "ERR-GGGCC" the
    // GGG is not a status group: blocks of serial numbers are handed out by
    // range — db-platform keeps 40000-40107 and 40300-40301, a configuration
    // gets 402xx — and ParseMessage reads the first three digits of one as the
    // code all the same. It also rewrites the identifier into the three-digit
    // shape, so by the time it reaches this function a serial block is
    // indistinguishable from a deliberate group: the distinction cannot be
    // made here, only where the code is minted. Such blocks are in use
    // (402xx, 410xx); none reaches this today: 402 is no member below, and
    // 410xx is minted by a v1 application, which maps codes to statuses
    // itself. Check the configurations in use before adding a member: one for
    // a number some configuration hands out as a serial block would start
    // relaying its business refusals as that status — silently, with no test
    // to catch it.
    // So: settle the legacy format before widening the enum.
    if (error_code >= 400) {
        if (const auto status = status_from_code(error_code))
            return *status;
    }

    return HttpStatus::bad_request;
}

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL
