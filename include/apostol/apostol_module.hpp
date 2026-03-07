#pragma once

#include "apostol/module.hpp"
#include "apostol/http.hpp"
#include "apostol/http_utils.hpp"
#include "apostol/oauth_providers.hpp"
#ifdef WITH_POSTGRESQL
#include "apostol/pg.hpp"
#include "apostol/pg_utils.hpp"
#endif

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── ApostolModule ───────────────────────────────────────────────────────────
//
// Base class for modules that dispatch HTTP requests by method.
//
// Mirrors v1 CApostolModule behaviour:
//   - init_methods() registers method handlers via add_method()
//   - check_location() filters requests by path (default: handle all)
//   - execute() calls check_location(), then dispatches by method
//   - OPTIONS / 405 are handled automatically
//   - CORS headers are injected before every matched handler
//   - Static helpers mirror v1 GetHost/GetOrigin/GetRealIP/ReplyError/Redirect
//
// Usage:
//   class MyModule final : public ApostolModule {
//   public:
//       MyModule() { add_allowed_origin("https://example.com"); }
//   protected:
//       void init_methods() override {
//           add_method("GET",  [this](auto& req, auto& resp) { do_get(req,  resp); });
//           add_method("POST", [this](auto& req, auto& resp) { do_post(req, resp); });
//       }
//   };
//
class ApostolModule : public Module
{
public:
    /// Dispatches HTTP requests by method. Calls check_location() first;
    /// returns false if the path doesn't match (next module gets a chance).
    /// Override check_location() to filter by path; override init_methods()
    /// to register method handlers.
    bool execute(const HttpRequest& req, HttpResponse& resp) override;

    /// Return true if this module should handle @p req.
    /// Default: true (handle all requests, like WebServer).
    /// Override to filter by path (e.g. starts_with("/oauth2/")).
    virtual bool check_location(const HttpRequest& req) const;

protected:
    using MethodFn = std::function<void(const HttpRequest&, HttpResponse&)>;

    // ── Method registration ───────────────────────────────────────────────────

    /// Override to register handlers via add_method().
    virtual void init_methods() = 0;

    /// Register @p name as an allowed method with the given handler.
    /// Call only from init_methods().
    void add_method(std::string name, MethodFn fn);

    // ── CORS ─────────────────────────────────────────────────────────────────
    //
    // Mirrors v1 CApostolModule CORS / IsOriginAllowed / GetAllowedHeaders /
    // LoadAllowedOrigins.
    //
    // Call add_allowed_origin() from the constructor (or init_methods()) to
    // enable CORS for specific origins.  Pass "*" to allow any origin.
    //
    // Alternatively call load_allowed_origins() with the conf/oauth2/ directory
    // to populate the allow-list automatically from the project's OAuth2 provider
    // JSON files — mirrors v1 CApostolModule::LoadAllowedOrigins() which reads
    // from Server().Providers().

    /// Add an origin to the allow-list (e.g. "https://example.com" or "*").
    void add_allowed_origin(std::string origin);

    /// Append a header to Access-Control-Allow-Headers.
    /// Default set: "Content-Type", "X-Requested-With" (mirrors v1 constructor).
    void add_allowed_header(std::string header);

    /// True when @p origin is in the allow-list, or "*" is present.
    bool is_origin_allowed(std::string_view origin) const;

    /// Load allowed origins from the centralized OAuthProviders cache.
    /// Adds each unique javascript_origin to the allow-list.
    /// Mirrors v1 CApostolModule::LoadAllowedOrigins() / Server().Providers().
    void load_allowed_origins(const OAuthProviders& providers);

    /// If the request carries an allowed Origin, add the four CORS headers.
    /// Called automatically by execute() before every dispatched handler and
    /// before do_options().
    void cors(const HttpRequest& req, HttpResponse& resp);

    // ── Default virtual handlers ──────────────────────────────────────────────

    /// Default OPTIONS response — 204 No Content with Allow header.
    /// Override for custom preflight body.
    virtual void do_options(const HttpRequest& req, HttpResponse& resp);

    /// Default 405 response — Allow header (RFC 7231 §7.4.1).
    virtual void method_not_allowed(const HttpRequest& req, HttpResponse& resp);

    // ── HTTP utilities ────────────────────────────────────────────────────────
    //
    // Mirrors v1 CApostolModule GetHost / GetOrigin / GetRealIP / GetProtocol /
    // Redirect / ReplyError.

    /// Return the X-Real-IP header value (or empty string if absent).
    static std::string get_real_ip  (const HttpRequest& req);

    /// Return the Origin header value (or empty string if absent).
    static std::string get_origin   (const HttpRequest& req);

    /// Return X-Forwarded-Proto header value, or "http" if absent.
    static std::string get_protocol (const HttpRequest& req);

    /// Return the Host header value, or "localhost" if absent.
    static std::string get_host     (const HttpRequest& req);

    /// Set a redirect response (default 302 Found).
    static void redirect(HttpResponse& resp,
                         std::string_view location,
                         HttpStatus code = HttpStatus::found);

    /// Set a JSON error body: {"error":{"code":<n>,"message":"<msg>"}}.
    static void reply_error(HttpResponse& resp,
                            HttpStatus      status,
                            std::string_view message);

    /// Overload taking a numeric status code (for codes not in HttpStatus).
    static void reply_error(HttpResponse& resp,
                            int             code,
                            std::string_view message);

    // ── Additional HTTP utilities (delegates to free functions) ──────────────

    /// Return User-Agent header value, or @p default_agent if absent.
    static std::string get_user_agent(const HttpRequest& req,
                                      std::string_view default_agent = "");

    /// Parse request body by Content-Type: JSON → parse, form → decode, else → params.
    static nlohmann::json content_to_json(const HttpRequest& req);

    /// Parse "Bearer <token>" or "Basic <base64(user:pass)>" from Authorization header.
    static Authorization parse_authorization(std::string_view header_value);

    /// URL-encode a string (RFC 3986 unreserved characters pass through).
    static std::string url_encode(std::string_view s);

    /// URL-decode: %XX → char, + → space.
    static std::string url_decode(std::string_view s);

    /// Escape special JSON characters: " \ \n \r \t.
    static std::string json_escape(std::string_view s);

    /// Check if @p path matches any pattern in @p patterns (trailing '*' glob).
    static bool match_path(std::string_view path,
                           const std::vector<std::string>& patterns);

    // ── File serving ──────────────────────────────────────────────────────────

    /// Open @p path, write its contents to @p resp (200 OK + MIME type).
    /// If head_only is true the body is suppressed (Content-Length still set).
    /// Returns false if the file cannot be opened.
    bool serve_file(const std::filesystem::path& path,
                    HttpResponse& resp, bool head_only);

    /// Map file extension (with leading dot, e.g. ".html") to MIME type.
    static std::string_view mime_type(const std::string& ext);

#ifdef WITH_POSTGRESQL
    // ── PostgreSQL utilities ──────────────────────────────────────────────────
    //
    // Mirrors v1 CApostolModule PQResultToJson / DoPostgresQueryExecuted.
    //
    // pg_result_to_json() formats a single PgResult as a JSON string.
    // The @p format parameter controls wrapping:
    //   ""  or absent  — auto: single row → plain value; multiple rows → array
    //   "array"        — always return a JSON array
    //   "null"         — empty result → literal "null" (not "{}" or "[]")
    // @p object_name, when set, wraps the result in {"<name>": ...}.

    static std::string pg_result_to_json(const PgResult&  result,
                                         std::string_view format      = "",
                                         std::string_view object_name = "");

    /// Set @p resp body from the first ok() result in @p results.
    /// Content-Type is set to application/json.
    /// On DB error (empty vector or !ok()), sets 500 + JSON error body.
    static void reply_pg(HttpResponse&                resp,
                         const std::vector<PgResult>& results,
                         std::string_view             format      = "",
                         std::string_view             object_name = "");

    // pg_sql_to_json() serializes a PgResult as a JSON array of objects
    // using column names as keys. Unlike pg_result_to_json() (which expects
    // col 0 to contain pre-built JSON), this builds JSON from raw SQL columns.
    // Numeric PG types (int2/4/8, float4/8, numeric, bool) are emitted unquoted.

    static std::string pg_sql_to_json(const PgResult& result);

    /// Like reply_pg() but for raw SQL results (no row_to_json() needed).
    /// Always returns a JSON array of objects.
    static void reply_sql(HttpResponse&                resp,
                          const std::vector<PgResult>& results);

    // ── PostgreSQL utility delegates ────────────────────────────────────────

    /// SQL-escape a string literal without PGconn* (manual E'...' escaping).
    static std::string pq_quote_literal(std::string_view val);

    /// Convert HTTP headers to a JSON object string: {"Name":"value",...}.
    static std::string headers_to_json(
        const std::vector<std::pair<std::string, std::string>>& headers);

    /// Convert URL query params to a JSON object string: {"key":"value",...}.
    static std::string params_to_json(
        const std::vector<std::pair<std::string, std::string>>& params);

    /// Parse {"error":{"code":N,"message":"M"}} from PG result JSON.
    /// Returns the error code (0 = no error).
    static int check_pg_error(std::string_view json, std::string& error_message);

    /// Map a PG/application error code to an HTTP status.
    static HttpStatus error_code_to_status(int error_code);
#endif // WITH_POSTGRESQL

private:
    struct MethodEntry
    {
        std::string name;
        MethodFn    fn;
    };

    std::vector<MethodEntry>    methods_;
    mutable std::string         allowed_cache_;
    bool                        initialized_ = false;

    std::vector<std::string>    allowed_origins_;
    // Default headers match v1 CApostolModule constructor initialisation
    std::vector<std::string>    allowed_headers_{"Content-Type", "X-Requested-With"};
    mutable std::string         allowed_headers_cache_;

    /// Comma-separated list of registered methods + "OPTIONS". Cached.
    std::string_view get_allowed() const;

    /// Comma-separated Access-Control-Allow-Headers string. Cached.
    const std::string& get_allowed_headers_str() const;
};

} // namespace apostol
