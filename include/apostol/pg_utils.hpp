#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/http.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apostol
{

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
