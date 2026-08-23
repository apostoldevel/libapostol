#ifdef WITH_SSL

#include "apostol/jwt.hpp"
#include "apostol/oauth_providers.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#define JWT_DISABLE_PICOJSON
#include "jwt-cpp/traits/nlohmann-json/traits.h"

namespace apostol
{

using jwt_traits = jwt::traits::nlohmann_json;

// ─── verify_jwt ──────────────────────────────────────────────────────────────

JwtClaims verify_jwt(std::string_view token,
                     const OAuthProviders& providers,
                     const JwtKeyResolver& key_resolver)
{
    // 1. Decode token (no verification yet) to extract header claims
    auto decoded = jwt::decode<jwt_traits>(std::string(token));

    std::string aud;
    std::string alg;
    std::string iss;

    try { aud = decoded.get_audience(); }
    catch (...) { throw JwtVerificationError("missing audience claim"); }

    try { alg = decoded.get_algorithm(); }
    catch (...) { throw JwtVerificationError("missing algorithm claim"); }

    try { iss = decoded.get_issuer(); }
    catch (...) { /* issuer is optional */ }

    // 2. Find provider by client_id == aud
    auto* app = providers.find_by_client_id(aud);
    if (!app)
        throw JwtVerificationError("unknown audience: " + aud);

    // 3. Check issuer if the provider has issuers configured
    if (!app->issuers.empty() && !iss.empty()) {
        bool issuer_ok = false;
        for (const auto& allowed : app->issuers)
            if (allowed == iss) { issuer_ok = true; break; }
        if (!issuer_ok)
            throw JwtVerificationError("issuer mismatch: " + iss);
    }

    // 4. Determine algorithm family
    const auto ch = alg.substr(0, 2);

    // 5. Verify signature
    try {
        if (ch == "HS") {
            const auto& secret = app->client_secret;
            if (secret.empty())
                throw JwtVerificationError("empty secret for audience: " + aud);

            if (alg == "HS256") {
                jwt::verify<jwt_traits>()
                    .allow_algorithm(jwt::algorithm::hs256{secret})
                    .verify(decoded);
            } else if (alg == "HS384") {
                jwt::verify<jwt_traits>()
                    .allow_algorithm(jwt::algorithm::hs384{secret})
                    .verify(decoded);
            } else if (alg == "HS512") {
                jwt::verify<jwt_traits>()
                    .allow_algorithm(jwt::algorithm::hs512{secret})
                    .verify(decoded);
            } else {
                throw JwtVerificationError("unsupported HS algorithm: " + alg);
            }
        } else if (ch == "RS" || ch == "ES" || ch == "PS") {
            if (!key_resolver)
                throw JwtVerificationError("asymmetric algorithm " + alg + " requires key_resolver");

            std::string kid;
            try { kid = decoded.get_key_id(); }
            catch (...) { throw JwtVerificationError("missing kid for asymmetric algorithm"); }

            auto key = key_resolver(kid);
            if (key.empty())
                throw JwtVerificationError("no public key for kid: " + kid);

            if (alg == "RS256") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::rs256{key}).verify(decoded);
            } else if (alg == "RS384") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::rs384{key}).verify(decoded);
            } else if (alg == "RS512") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::rs512{key}).verify(decoded);
            } else if (alg == "ES256") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::es256{key}).verify(decoded);
            } else if (alg == "ES384") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::es384{key}).verify(decoded);
            } else if (alg == "ES512") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::es512{key}).verify(decoded);
            } else if (alg == "PS256") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::ps256{key}).verify(decoded);
            } else if (alg == "PS384") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::ps384{key}).verify(decoded);
            } else if (alg == "PS512") {
                jwt::verify<jwt_traits>().allow_algorithm(jwt::algorithm::ps512{key}).verify(decoded);
            } else {
                throw JwtVerificationError("unsupported algorithm: " + alg);
            }
        } else {
            throw JwtVerificationError("unsupported algorithm: " + alg);
        }
    } catch (const jwt::error::token_expired_exception&) {
        throw JwtExpiredError("token expired");
    } catch (const jwt::error::signature_verification_exception& e) {
        throw JwtVerificationError(std::string("signature verification failed: ") + e.what());
    } catch (const jwt::error::token_verification_exception& e) {
        throw JwtVerificationError(std::string("token verification failed: ") + e.what());
    } catch (const JwtExpiredError&) {
        throw; // re-throw our own
    } catch (const JwtVerificationError&) {
        throw; // re-throw our own
    } catch (const std::exception& e) {
        throw JwtVerificationError(std::string("verification failed: ") + e.what());
    }

    // 6. Extract claims
    JwtClaims claims;
    try { claims.sub = decoded.get_subject(); }
    catch (...) { /* sub may be absent */ }

    claims.aud = std::move(aud);
    claims.iss = std::move(iss);

    return claims;
}

// ─── create_jwt ──────────────────────────────────────────────────────────────

std::string create_jwt(const OAuthProviders& providers,
                       int expires_in_secs)
{
    auto* app = providers.find_default("web");
    if (!app)
        throw JwtVerificationError("default web app not found");

    if (app->client_secret.empty())
        throw JwtVerificationError("empty secret for default web app");

    std::string issuer;
    if (!app->issuers.empty())
        issuer = app->issuers.front();

    auto now = std::chrono::system_clock::now();
    return jwt::create<jwt_traits>()
        .set_algorithm("HS256")
        .set_issuer(issuer)
        .set_audience(app->client_id)
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(expires_in_secs))
        .sign(jwt::algorithm::hs256{app->client_secret});
}

// ─── verify_and_resign_jwt ───────────────────────────────────────────────────

std::string verify_and_resign_jwt(std::string_view token,
                                  const OAuthProviders& providers,
                                  const JwtKeyResolver& key_resolver)
{
    // Decode without verification first to check algorithm
    auto decoded = jwt::decode<jwt_traits>(std::string(token));
    auto alg = decoded.get_algorithm();

    // HS256 tokens are already signed with the provider's secret — return as-is
    if (alg == "HS256") {
        // Still verify it
        verify_jwt(token, providers, key_resolver);
        return std::string(token);
    }

    // Verify the token (may use asymmetric key)
    auto claims = verify_jwt(token, providers, key_resolver);

    // Find the matching provider to get the secret for re-signing
    auto* app = providers.find_by_client_id(claims.aud);
    if (!app)
        throw JwtVerificationError("unknown audience: " + claims.aud);

    if (app->client_secret.empty())
        throw JwtVerificationError("empty secret for audience: " + claims.aud);

    // Re-sign the payload as HS256 with the provider's secret.
    // Mirrors v1 CCleanToken: copy all payload claims into a new HS256 token.
    auto builder = jwt::create<jwt_traits>()
        .set_algorithm("HS256");

    for (const auto& [key, val] : decoded.get_payload_json()) {
        builder.set_payload_claim(key, jwt::basic_claim<jwt_traits>(val));
    }

    return builder.sign(jwt::algorithm::hs256{app->client_secret});
}

// ─── sign_claims_jwt ─────────────────────────────────────────────────────────

std::string sign_claims_jwt(const OAuthApp& app,
                            std::string_view claims_json,
                            std::string_view subject,
                            int expires_in_secs)
{
    if (app.client_secret.empty())
        throw JwtVerificationError("empty secret for audience: " + app.client_id);

    if (app.issuers.empty())
        throw JwtVerificationError("no issuer configured for provider: " + app.provider);

    nlohmann::json claims;
    try {
        claims = nlohmann::json::parse(claims_json);
    } catch (const std::exception& e) {
        throw JwtVerificationError(std::string("claims are not JSON: ") + e.what());
    }

    if (!claims.is_object())
        throw JwtVerificationError("claims are not a JSON object");

    auto builder = jwt::create<jwt_traits>().set_algorithm("HS256");

    // The provider's own claim names travel unchanged: which of them is the
    // address and which the given name is written down in the database, per
    // provider, and duplicating that map here would be two places to keep in
    // step. The registered claims below are set afterwards so that a provider
    // sending its own iss or aud cannot decide either.
    for (const auto& [key, val] : claims.items()) {
        if (key == "iss" || key == "aud" || key == "sub" ||
            key == "iat" || key == "exp")
            continue;
        builder.set_payload_claim(key, jwt::basic_claim<jwt_traits>(val));
    }

    auto now = std::chrono::system_clock::now();

    return builder
        .set_issuer(app.issuers.front())
        .set_audience(app.client_id)
        .set_subject(std::string(subject))
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(expires_in_secs))
        .sign(jwt::algorithm::hs256{app.client_secret});
}

// ─── hmac_sha256_hex ─────────────────────────────────────────────────────────

std::string hmac_sha256_hex(std::string_view key, std::string_view data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_len);

    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        result.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
        result.push_back(hex_chars[digest[i] & 0x0F]);
    }
    return result;
}

} // namespace apostol

#endif // WITH_SSL
