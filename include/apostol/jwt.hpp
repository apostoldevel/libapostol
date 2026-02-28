#pragma once

#ifdef WITH_SSL

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace apostol
{

class OAuthProviders; // forward declaration

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

/// Optional callback to resolve public key by kid (for RS/ES/PS algorithms).
/// Returns PEM-encoded public key string.
using JwtKeyResolver = std::function<std::string(std::string_view kid)>;

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

/// Compute HMAC-SHA256 and return the result as a lowercase hex string.
/// Uses OpenSSL HMAC(). Used by WebSocketAPI for signed_fetch.
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

} // namespace apostol

#endif // WITH_SSL
