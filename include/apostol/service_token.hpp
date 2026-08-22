#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/pg.hpp"

#include <chrono>
#include <string>

namespace apostol
{

// ─── ServiceToken ────────────────────────────────────────────────────────────
//
// An OAuth 2.0 access token the application holds for itself, obtained with the
// client credentials grant and kept fresh in the background.
//
// This exists so that a browser never has to. RFC 6749 §2.1 classifies a
// browser-based application as a *public client* — "incapable of maintaining the
// confidentiality of their credentials" — and §4.4 states that "the client
// credentials grant type MUST only be used by confidential clients". A backend
// module holding a secret from conf/oauth2 is a confidential client and may run
// that grant; the page it serves may not, whatever precautions the page takes.
//
// Use it for the work a caller cannot authenticate for yet — checking whether an
// identifier is taken, registering, recovering a password — where the privilege
// belongs to the server rather than to whoever happens to be asking.
//
// Three things in this tree obtain tokens; pick deliberately:
//
//   BotSession    — a *session* via api.login/api.get_session, for acting as a
//                   user (apibot). A session code is not a JWT, and platform
//                   functions calling TokenValidation will not accept one.
//   ServiceToken  — this: a JWT access token from the local daemon.token.
//   ReplicationServer::refresh_token — the same grant against a *remote* server
//                   over HTTP, with its own backoff policy.
//
// Threading: single-threaded, like everything on the event loop.
// refresh_if_needed() is meant to be called from a module's or process's
// heartbeat(); sign_out() from on_stop().
//
class ServiceToken
{
public:
    ServiceToken(PgPool& pool, std::string agent, std::string host);

    /// Credentials of a *confidential* client — typically
    /// providers().credentials("service"). Must be set before refresh_if_needed().
    /// An empty scope asks the authorization server for its default; prefer naming
    /// one, because the token's scope decides what it may reach.
    void set_credentials(std::string client_id, std::string client_secret,
                         std::string scope = {});

    /// True while a token is held and the authorization server would still accept
    /// it. Distinct from being due for renewal: a token stays usable through the
    /// renewal margin, which matters when the database is briefly unreachable.
    bool valid() const noexcept;

    /// The access token, or empty when none is held. Check valid() first.
    const std::string& token() const noexcept { return token_; }

    /// Drop the token held. Call when the server has rejected it — the next
    /// heartbeat then obtains a fresh one instead of waiting out its nominal life.
    void invalidate() noexcept;

    /// Call from heartbeat(). Obtains a token when there is none, or when the one
    /// held is close enough to expiry to be worth replacing. Failures back off.
    void refresh_if_needed();

    /// Close the session behind the current token. Call from on_stop(): every
    /// client_credentials grant creates a row in db.session, and nothing collects
    /// them.
    void sign_out();

private:
    void schedule_retry();

    PgPool&     pool_;
    std::string agent_;
    std::string host_;
    std::string client_id_;
    std::string client_secret_;
    std::string scope_;
    std::string token_;
    std::string session_;

    // Two marks, not one. hard_expiry_ is when the server stops accepting the
    // token; renew_at_ is when we start trying to replace it. Collapsing them
    // throws away the margin exactly when it is needed — while refreshes are
    // failing.
    std::chrono::steady_clock::time_point hard_expiry_{};
    std::chrono::steady_clock::time_point renew_at_{};
    std::chrono::steady_clock::time_point retry_at_{};
    std::chrono::steady_clock::time_point refresh_deadline_{};

    std::chrono::seconds backoff_{0};
    bool refreshing_{false};

    static constexpr auto k_renew_margin    = std::chrono::seconds(300);
    static constexpr auto k_fallback_life   = std::chrono::seconds(3600);
    static constexpr auto k_min_life        = std::chrono::seconds(30);
    static constexpr auto k_max_life        = std::chrono::seconds(30 * 24 * 3600);
    static constexpr auto k_retry_initial   = std::chrono::seconds(10);
    static constexpr auto k_retry_max       = std::chrono::seconds(1800);
    static constexpr auto k_refresh_timeout = std::chrono::seconds(30);
};

} // namespace apostol

#endif // WITH_POSTGRESQL
