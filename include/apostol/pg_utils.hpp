#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/http.hpp"
#include "apostol/pg.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apostol
{

// ── PG result → JSON ─────────────────────────────────────────────────────────
//
// Mirrors v1 CApostolModule PQResultToJson / DoPostgresQueryExecuted.

/// Format a single PgResult as a JSON string.
/// @p format controls wrapping:
///   ""  or absent  — auto: single row → plain value; multiple rows → array
///   "array"        — always return a JSON array
///   "null"         — empty result → literal "null" (not "{}" or "[]")
/// @p object_name, when set, wraps the result in {"<name>": ...}.
std::string pg_result_to_json(const PgResult&  result,
                              std::string_view format      = "",
                              std::string_view object_name = "");

/// Set @p resp body from the first ok() result in @p results.
/// Content-Type is set to application/json.
/// On DB error (empty vector or !ok()), sets 500 + JSON error body.
void reply_pg(HttpResponse&                resp,
              const std::vector<PgResult>& results,
              std::string_view             format      = "",
              std::string_view             object_name = "");

/// Serialize a PgResult as a JSON array of objects using column names as keys.
/// Unlike pg_result_to_json() (which expects col 0 to contain pre-built JSON),
/// this builds JSON from raw SQL columns.
/// Numeric PG types (int2/4/8, float4/8, numeric, bool) are emitted unquoted.
std::string pg_sql_to_json(const PgResult& result);

/// Like reply_pg() but for raw SQL results (no row_to_json() needed).
/// Always returns a JSON array of objects.
void reply_sql(HttpResponse&                resp,
               const std::vector<PgResult>& results);

// ── PG scalar utilities ──────────────────────────────────────────────────────

/// SQL-escape a string literal without PGconn* (manual E'...' escaping).
/// Mirrors v1 PQQuoteLiteral().
///
/// A NUL byte is copied as it is: no text value can hold one, and a statement
/// is sent as a C string, so it would end there. Whoever sends the statement
/// does not send it with one: exec_sql() answers 400 instead.
std::string pq_quote_literal(std::string_view val);

/// SQL-quote an identifier without PGconn*: wrap in double quotes, double
/// every double quote inside ("a""b"). Always quoted, so case and spaces
/// survive as given. An empty name gives "" — which the server rejects, as it
/// should: there is no such identifier.
std::string pq_quote_ident(std::string_view name);

// ── db-platform request and response contract — deprecated here ─────────────
//
// Forwarders kept for their callers; the functions and their documentation
// are db_platform::… in db_platform_response.hpp.

std::string headers_to_json(
    const std::vector<std::pair<std::string, std::string>>& headers);
std::string params_to_json(
    const std::vector<std::pair<std::string, std::string>>& params);
int check_pg_error(std::string_view json, std::string& error_message);
int check_pg_error(std::string_view json, std::string& error_message,
                   std::string& error_id);
HttpStatus error_code_to_status(int error_code);

/// URL-encode form params to a JSON object string.
/// Input: "key1=val1&key2=val2" → {"key1":"val1","key2":"val2"}.
std::string form_to_json(std::string_view form_body);

} // namespace apostol

#endif // WITH_POSTGRESQL
