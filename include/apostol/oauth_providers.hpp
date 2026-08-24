#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apostol
{

/// Single OAuth2 application entry (one section inside a provider JSON file).
struct OAuthApp
{
    std::string provider;           // filename without extension ("default", "google")
    std::string name;               // section name ("web", "service", "android", "ios")
    std::string client_id;          // unique identifier
    std::string client_secret;      // secret for JWT signing / OAuth2
    std::string algorithm;          // "HS256", "HS384", "HS512"
    std::vector<std::string> issuers;            // JWT iss claim values
    std::vector<std::string> javascript_origins; // CORS origins
    std::vector<std::string> redirect_uris;      // allowed redirect URIs
    std::vector<std::string> allowed_ips;         // IP whitelist (empty = any)
    std::vector<std::string> scopes;             // allowed OAuth2 scopes
    std::string auth_uri;           // authorization endpoint
    std::string token_uri;          // token exchange endpoint
    std::string cert_uri;           // JWKS public key endpoint

    // For a provider that issues no id_token, the profile is fetched from
    // userinfo_uri instead. That answer also has to say which application the
    // token was issued to — otherwise a token obtained by any other client of
    // the same provider signs its bearer in as our user, which is precisely
    // what the aud claim of an id_token prevents. userinfo_audience names the
    // field carrying it; without both, the provider has no such branch.
    std::string userinfo_uri;       // userinfo endpoint (providers without id_token)
    std::string userinfo_audience;  // field in that answer holding the client_id
    std::string userinfo_subject;   // field holding the user id (default "sub")
    std::string userinfo_scheme;    // Authorization scheme for it (default "Bearer")

    // Fields for the sign-in provider list an unauthenticated login screen fetches.
    // They describe how to *start* a sign-in at this provider and how to label it —
    // none of them is a secret, and none overlaps with the credentials above. A
    // provider that does not set `external` is not one of these: `default` and
    // `bridge` are this installation's own applications, whose auth_uri points back
    // at us, and "sign in through ourselves" is not a button. login_scope is the
    // provider's OAuth scope string (e.g. Yandex "login:email login:info"), which is
    // unrelated to `scopes` above — those are our own db.scope codes.
    std::string login_scope;        // OAuth scope to request at the provider
    std::string display_name;       // label for the button ("Yandex", "Google")
    std::string icon;               // icon hint for the button (name or data URI)
    bool        external = false;    // true = an external sign-in provider, listed
};

// ─── OAuthProviders ──────────────────────────────────────────────────────────
//
// Centralized cache of OAuth2 provider configurations loaded from conf/oauth2/*.json.
//
// Replaces three separate filesystem scans:
//   - Application::load_oauth2_credentials()  (BotSession)
//   - verify_jwt() per-request scan            (JWT verification)
//   - ApostolModule::load_allowed_origins()    (CORS)
//
// Loaded once at startup, reloaded on SIGHUP via clear() + load().
//
class OAuthProviders
{
public:
    /// Load all *.json files from @p oauth2_dir.
    /// Each file may contain multiple application sections.
    /// Non-existent directory or malformed files are silently skipped.
    void load(const std::filesystem::path& oauth2_dir);

    /// Clear all loaded applications (call before reload).
    void clear();

    /// All loaded applications.
    const std::vector<OAuthApp>& apps() const noexcept { return apps_; }

    /// Find an application by its client_id (used for JWT verification).
    /// Searches every provider, so a hit says only that the client_id is
    /// registered somewhere — not that it is a client of ours. Returns nullptr
    /// if not found.
    const OAuthApp* find_by_client_id(std::string_view client_id) const;

    /// Find an application by client_id, restricted to a single provider.
    /// Returns nullptr if not found.
    const OAuthApp* find_by_client_id(std::string_view client_id,
                                      std::string_view provider) const;

    /// Find an application by client_id among clients of the "default" provider —
    /// the ones this installation itself issues credentials for. Companion to
    /// find_default(app_name) above. Use this, not the unrestricted overload,
    /// wherever the answer decides what a client may do on behalf of a local user:
    /// an entry under google or yandex is registration for verifying that
    /// provider's tokens, and gives its holder no standing here.
    const OAuthApp* find_default_by_client_id(std::string_view client_id) const;

    /// Find app by provider name + app name (e.g. "google", "web").
    const OAuthApp* find(std::string_view provider, std::string_view app_name) const;

    /// Find app of the "default" provider by app name (e.g. "web", "service").
    const OAuthApp* find_default(std::string_view app_name) const;

    /// Find credentials {client_id, client_secret} for a named application
    /// (e.g. "service", "web"). Returns {"",""} if not found.
    std::pair<std::string, std::string> credentials(std::string_view app_name) const;

    /// Collect all unique javascript_origins across all applications.
    std::vector<std::string> allowed_origins() const;

private:
    std::vector<OAuthApp> apps_;
};

} // namespace apostol
