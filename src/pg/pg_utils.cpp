#ifdef WITH_POSTGRESQL

#include "apostol/pg_utils.hpp"
#include "apostol/db_platform_response.hpp"
#include "apostol/http_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

// ─── pg_result_to_json ──────────────────────────────────────────────────────

std::string pg_result_to_json(const PgResult&  result,
                              std::string_view format,
                              std::string_view object_name)
{
    const bool wrap_object = !object_name.empty();
    const bool as_array    = wrap_object || format == "array" || result.rows() > 1;
    const char* empty_data = as_array ? "[]" : "{}";

    if (result.rows() == 0) {
        if (format == "null")
            return "null";
        if (wrap_object)
            return fmt::format("{{\"{}\":{}}}", object_name, empty_data);
        return empty_data;
    }

    std::string json;
    json.reserve(512);

    if (wrap_object)
        json += fmt::format("{{\"{}\":", object_name);

    if (as_array)
        json += '[';

    for (int row = 0; row < result.rows(); ++row) {
        if (row > 0) json += ',';
        if (!result.is_null(row, 0)) {
            json += result.value(row, 0);
        } else {
            json += as_array ? "null" : "{}";
        }
    }

    if (as_array)
        json += ']';

    if (wrap_object)
        json += '}';

    return json;
}

// ─── reply_pg ───────────────────────────────────────────────────────────────

void reply_pg(HttpResponse&                resp,
              const std::vector<PgResult>& results,
              std::string_view             format,
              std::string_view             object_name)
{
    if (results.empty()) {
        reply_error(resp, HttpStatus::internal_server_error, "empty result set");
        return;
    }

    const auto& first = results.front();
    if (!first.ok()) {
        const char* err = first.error_message();
        reply_error(resp, HttpStatus::internal_server_error,
                    (err && *err) ? err : "query failed");
        return;
    }

    resp.set_status(HttpStatus::ok)
        .set_body(pg_result_to_json(first, format, object_name),
                  "application/json");
}

// ─── pg_sql_to_json ─────────────────────────────────────────────────────────

// PG type OIDs (stable across all PG versions).
static constexpr Oid kBoolOid    = 16;
static constexpr Oid kInt2Oid    = 21;
static constexpr Oid kInt4Oid    = 23;
static constexpr Oid kInt8Oid    = 20;
static constexpr Oid kOidOid     = 26;
static constexpr Oid kFloat4Oid  = 700;
static constexpr Oid kFloat8Oid  = 701;
static constexpr Oid kNumericOid = 1700;
static constexpr Oid kJsonOid    = 114;
static constexpr Oid kJsonbOid   = 3802;

std::string pg_sql_to_json(const PgResult& result)
{
    const int nrows = result.rows();
    const int ncols = result.columns();

    std::string json;
    json.reserve(nrows * ncols * 32);
    json += '[';

    for (int r = 0; r < nrows; ++r) {
        if (r > 0) json += ',';
        json += '{';
        for (int c = 0; c < ncols; ++c) {
            if (c > 0) json += ',';
            json += '"';
            json += json_escape(result.column_name(c));
            json += "\":";

            if (result.is_null(r, c)) {
                json += "null";
                continue;
            }

            const Oid type = result.column_type(c);
            const char* val = result.value(r, c);

            if (type == kBoolOid) {
                json += (val[0] == 't') ? "true" : "false";
            } else if (type == kJsonOid || type == kJsonbOid) {
                json += val;
            } else if (type == kInt2Oid || type == kInt4Oid ||
                       type == kInt8Oid || type == kOidOid) {
                json += val;
            } else if (type == kFloat4Oid || type == kFloat8Oid || type == kNumericOid) {
                json += val;
            } else {
                json += '"';
                json += json_escape(val);
                json += '"';
            }
        }
        json += '}';
    }

    json += ']';
    return json;
}

// ─── reply_sql ──────────────────────────────────────────────────────────────

void reply_sql(HttpResponse&                resp,
               const std::vector<PgResult>& results)
{
    if (results.empty()) {
        reply_error(resp, HttpStatus::internal_server_error, "empty result set");
        return;
    }

    const auto& first = results.front();
    if (!first.ok()) {
        const char* err = first.error_message();
        reply_error(resp, HttpStatus::internal_server_error,
                    (err && *err) ? err : "query failed");
        return;
    }

    resp.set_status(HttpStatus::ok)
        .set_body(pg_sql_to_json(first), "application/json");
}

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

// ─── pq_quote_ident ─────────────────────────────────────────────────────────

std::string pq_quote_ident(std::string_view name)
{
    std::string out;
    out.reserve(name.size() + 2);
    out += '"';
    for (char c : name) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

// ─── form_to_json ───────────────────────────────────────────────────────────
// Delegates to parse_form_body() from http_utils.hpp (url_decode is also there).

std::string form_to_json(std::string_view form_body)
{
    return parse_form_body(form_body).dump();
}

// ─── db-platform contract: forwarders ───────────────────────────────────────
// Deprecated: the implementations are db_platform::… (db_platform_response.hpp).

std::string headers_to_json(
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    return db_platform::headers_to_json(headers);
}

std::string params_to_json(
    const std::vector<std::pair<std::string, std::string>>& params)
{
    return db_platform::params_to_json(params);
}

int check_pg_error(std::string_view json, std::string& error_message)
{
    return db_platform::check_pg_error(json, error_message);
}

int check_pg_error(std::string_view json, std::string& error_message,
                   std::string& error_id)
{
    return db_platform::check_pg_error(json, error_message, error_id);
}

HttpStatus error_code_to_status(int error_code)
{
    return db_platform::error_code_to_status(error_code);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
