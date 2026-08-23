#pragma once

#include "apostol/http.hpp"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── Path matching ──────────────────────────────────────────────────────────

/// Check if @p path matches any pattern in @p patterns.
/// Trailing '*' glob: "/api/*" matches "/api/anything". Exact match otherwise.
bool match_path(std::string_view path, const std::vector<std::string>& patterns);

// ─── JSON escaping ──────────────────────────────────────────────────────────

/// Escape special JSON characters: " \ \n \r \t for safe embedding in JSON strings.
std::string json_escape(std::string_view s);

// ─── Error responses ────────────────────────────────────────────────────────

/// Set a JSON error body: {"error":{"code":<N>,"message":"<escaped msg>"}}.
void reply_error(HttpResponse& resp, HttpStatus status, std::string_view message);

/// Overload taking a numeric status code (for codes not in HttpStatus enum).
void reply_error(HttpResponse& resp, int code, std::string_view message);

/// Reply to a failed bearer token: the same JSON body as reply_error, plus the
/// challenge RFC 6750 §3 requires of a resource server that refuses one.
///
/// Without the header a client is told only that it may not pass, not why or what
/// to do: §3.1 separates invalid_token — expired, revoked, malformed, "invalid for
/// any other reason" — which is worth obtaining a new token over, from
/// insufficient_scope, which is not. A client that reads the challenge to decide
/// whether to refresh finds nothing to read.
///
/// @p error is normalised to the three codes §3 defines; anything else becomes
/// invalid_token, which is what a refused bearer token means when it means
/// anything.
void reply_bearer_error(HttpResponse& resp, HttpStatus status,
                        std::string_view error, std::string_view description);

/// Add that challenge to a response whose body is already written — a 401 being
/// forwarded from the database, say, where the body is the answer and only the
/// header is missing. RFC 6750 §3 wants the challenge on every 401 a resource
/// server sends, not only the ones it composes itself.
void set_bearer_challenge(HttpResponse& resp, std::string_view error,
                          std::string_view description);

// ─── HTTP header utilities ──────────────────────────────────────────────────

/// Return X-Real-IP header value, or empty string if absent.
std::string get_real_ip(const HttpRequest& req);

/// Return Origin header value, or empty string if absent.
std::string get_origin(const HttpRequest& req);

/// Return X-Forwarded-Proto header value, or "http" if absent.
std::string get_protocol(const HttpRequest& req);

/// Return Host header value, or "localhost" if absent.
std::string get_host(const HttpRequest& req);

/// Return User-Agent header value, or @p default_agent if absent.
std::string get_user_agent(const HttpRequest& req, std::string_view default_agent = "");

/// Return system hostname via gethostname(2), fallback "localhost".
std::string get_hostname();

/// Set a redirect response (default 302 Found).
void redirect(HttpResponse& resp, std::string_view location,
              HttpStatus code = HttpStatus::found);

// ─── IP classification ──────────────────────────────────────────────────────

/// True if @p ip is loopback (127.x, ::1) or RFC 1918 private (10.x, 172.16-31.x, 192.168.x).
bool is_private_ip(std::string_view ip);

// ─── Authorization parsing ──────────────────────────────────────────────────

/// Parsed Authorization header value.
struct Authorization
{
    enum class Schema { none, basic, bearer };
    Schema schema = Schema::none;
    std::string token;     // Bearer token
    std::string username;  // Basic username (decoded)
    std::string password;  // Basic password (decoded)
};

/// Parse "Bearer <token>" or "Basic <base64(user:pass)>" from
/// the Authorization header value.
Authorization parse_authorization(std::string_view header_value);

// ─── URL encoding / decoding ────────────────────────────────────────────────

/// URL-encode a string (RFC 3986 unreserved characters pass through).
std::string url_encode(std::string_view s);

/// URL-decode: %XX -> char, + -> space. No PG dependency.
std::string url_decode(std::string_view s);

/// Parse "key1=val1&key2=val2" -> nlohmann::json object.
nlohmann::json parse_form_body(std::string_view body);

// ─── Content parsing ────────────────────────────────────────────────────────

/// Parse request body by Content-Type: JSON -> parse, form -> decode, else -> params.
nlohmann::json content_to_json(const HttpRequest& req);

} // namespace apostol
