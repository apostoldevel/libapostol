#ifdef WITH_POSTGRESQL

#include "apostol/pg_utils.hpp"
#include "apostol/http_utils.hpp"

#include <nlohmann/json.hpp>

namespace apostol
{

// ─── pq_quote_literal ───────────────────────────────────────────────────────

std::string pq_quote_literal(std::string_view val)
{
    std::string out;
    out.reserve(val.size() + 4);
    out += "E'";
    for (char c : val) {
        switch (c) {
            case '\'': out += "''";   break;
            case '\\': out += "\\\\"; break;
            default:   out += c;      break;
        }
    }
    out += '\'';
    return out;
}

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

// ─── form_to_json ───────────────────────────────────────────────────────────
// Delegates to parse_form_body() from http_utils.hpp (url_decode is also there).

std::string form_to_json(std::string_view form_body)
{
    return parse_form_body(form_body).dump();
}

// ─── check_pg_error ─────────────────────────────────────────────────────────

int check_pg_error(std::string_view json, std::string& error_message)
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

    switch (error_code) {
        case 401: return HttpStatus::unauthorized;
        case 403: return HttpStatus::forbidden;
        case 404: return HttpStatus::not_found;
        case 500: return HttpStatus::internal_server_error;
        default:  return HttpStatus::bad_request;
    }
}

} // namespace apostol

#endif // WITH_POSTGRESQL
