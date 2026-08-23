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
std::string pq_quote_literal(std::string_view val);

/// Convert HTTP headers to a JSON object string: {"Name":"value",...}.
/// Mirrors v1 FetchCommon::HeadersToJson().
std::string headers_to_json(
    const std::vector<std::pair<std::string, std::string>>& headers);

/// Convert URL query params to a JSON object string: {"key":"value",...}.
/// Mirrors v1 FetchCommon::ParamsToJson().
std::string params_to_json(
    const std::vector<std::pair<std::string, std::string>>& params);

/// Parse {"error":{"code":N,"message":"M"}} from PG result JSON.
/// Returns the error code (0 = no error).
/// Mirrors v1 CFetchCommon::CheckError().
int check_pg_error(std::string_view json, std::string& error_message);

/// As above, and also reports the error identifier — "ERR-401-008" and the like.
///
/// The status alone does not say what happened: an expired token and a locked
/// account both answer 401, and a caller that must decide whether to refresh a
/// token or send its user to sign in cannot tell them apart from the number. The
/// identifier is what db-platform's exception handlers put in the "error" field.
///
/// @p error_id is left empty when the payload carries no identifier. That is not
/// an error: the OAuth 2.0 responses of daemon.token and daemon.authorization_code
/// use the same field for an RFC 6749 §5.2 error code — "invalid_grant" and its
/// kin — and this reports only what looks like an identifier, so an OAuth code
/// never arrives disguised as one.
int check_pg_error(std::string_view json, std::string& error_message,
                   std::string& error_id);

/// Map a PG/application error code to an HTTP status.
/// Mirrors v1 ErrorCodeToStatus().
///   - code >= 10000: divide by 100 (e.g. 40100 → 401)
///   - 401,403,404,500 → respective HttpStatus
///   - else → 400 bad_request
HttpStatus error_code_to_status(int error_code);

/// URL-encode form params to a JSON object string.
/// Input: "key1=val1&key2=val2" → {"key1":"val1","key2":"val2"}.
std::string form_to_json(std::string_view form_body);

} // namespace apostol

#endif // WITH_POSTGRESQL
