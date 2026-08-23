#pragma once

#ifdef WITH_SSL

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace apostol
{

class OAuthProviders; // forward declaration
struct OAuthApp;      // forward declaration

/// Thrown when a JWT token has expired.
struct JwtExpiredError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Thrown when JWT signature verification fails or token is otherwise invalid.
struct JwtVerificationError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Extracted JWT claims.
struct JwtClaims
{
    std::string sub;   // subject (session ID)
    std::string aud;   // audience (OAuth2 client ID)
    std::string iss;   // issuer
};

/// Optional callback to resolve a public key for RS/ES/PS algorithms.
/// Returns a PEM-encoded public key, or an empty string when there is none.
///
/// Takes the provider as well as the kid, and is expected to look only within
/// that provider's keys. A kid is unique to whoever minted it and to nobody
/// else: searching every provider's key set means one provider's key can verify
/// a token claiming another's audience, which is the audience check undone.
using JwtKeyResolver =
    std::function<std::string(std::string_view kid, std::string_view provider)>;

/// Verify a JWT token against the centralized OAuth2 provider cache.
///
/// Finds the provider whose client_id matches the token's "aud" claim,
/// and verifies the signature using the provider's client_secret (HS*)
/// or the public key from key_resolver (RS*/ES*/PS*).
///
/// Supports HS256/384/512, RS256/384/512, ES256/384/512, PS256/384/512.
///
/// Also checks issuer: if the provider has issuers configured, the token's
/// iss claim must match one of them.
///
/// The key resolver is called with the provider the audience selected, so a
/// key is only ever accepted from the provider the token claims to come from.
///
/// @throws JwtExpiredError        if the token has expired
/// @throws JwtVerificationError   if signature is invalid, audience unknown, etc.
JwtClaims verify_jwt(std::string_view token,
                     const OAuthProviders& providers,
                     const JwtKeyResolver& key_resolver = {});

/// Create a short-lived HS256 JWT signed with the default "web" app secret.
/// Returns the encoded token string.
std::string create_jwt(const OAuthProviders& providers,
                       int expires_in_secs = 3600);

/// Verify token (any algorithm), then re-sign payload as HS256
/// with the matching provider's app secret. Used by Login flow
/// to convert external provider tokens to internal HS256 tokens.
/// HS256 tokens are returned as-is (already correct algorithm).
std::string verify_and_resign_jwt(std::string_view token,
                                  const OAuthProviders& providers,
                                  const JwtKeyResolver& key_resolver = {});

/// Sign an arbitrary claim set as an HS256 JWT with @p app's own secret.
///
/// For a provider that issues no id_token: the profile arrives as plain JSON
/// from a userinfo endpoint, and the SQL layer accepts only a JWT signed with
/// the audience's secret. The claims are carried across unchanged, under the
/// provider's own names — mapping them is the database's job, not this one's —
/// with iss, aud, sub, iat and exp set here.
///
/// @param claims_json  Object to carry as the payload; its own iss/aud/sub/iat/
///                     exp are overwritten.
/// @param subject      The provider's identifier for the user (sub claim).
///
/// @throws JwtVerificationError if @p app has no secret, no issuer, or
///         @p claims_json is not a JSON object.
std::string sign_claims_jwt(const OAuthApp& app,
                            std::string_view claims_json,
                            std::string_view subject,
                            int expires_in_secs = 300);

/// Compute HMAC-SHA256 and return the result as a lowercase hex string.
/// Uses OpenSSL HMAC(). Used by WebSocketAPI for signed_fetch.
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

} // namespace apostol

#endif // WITH_SSL
