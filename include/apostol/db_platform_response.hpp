#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/http.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apostol::db_platform
{

// ─── db-platform request and response contract ──────────────────────────────
//
// How a module hands an HTTP request to a db-platform function (headers and
// query parameters as JSON objects) and reads the answer: the error envelope
// {"error":{"code":N,"message":"M","error":"ERR-..."}} and the HTTP status its
// code stands for.
//
// Built with WITH_POSTGRESQL, not only with WITH_DB_PLATFORM: the forwarders
// of the same names in pg_utils.hpp and ApostolModule work in a build without
// the rest of the layer.

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
///   - code >= 10000: divide by 100 (e.g. 40100 → 401)
///   - a 4xx/5xx status this library knows → itself (409 and 503 included)
///   - anything else, 2xx and 3xx among it → 400 bad_request
///
/// What a caller must be able to trust is that the refusal it reads on the
/// status line is the one the database declared. The note at the definition
/// says why only refusals are relayed, and where the "the code is a status"
/// premise does not hold — read it before adding a member to HttpStatus.
///
/// v1's ErrorCodeToStatus(), which this once mirrored, has neither the /100
/// rule nor anything past the four codes above; a v1 and a v2 application answer the
/// same database refusal with different statuses.
HttpStatus error_code_to_status(int error_code);

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL
