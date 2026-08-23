#pragma once

#include <chrono>
#include <string>

namespace apostol
{

// ─── ServiceToken ────────────────────────────────────────────────────────────
//
// The lifecycle of an access token an application holds for itself: when it is
// still good, when to start replacing it, how long to wait after a failure, and
// when to give up on a request that never came back.
//
// It exists so that a browser never has to hold one. RFC 6749 §2.1 classifies a
// browser-based application as a *public client* — "incapable of maintaining the
// confidentiality of their credentials" — and §4.4 reserves the client credentials
// grant to confidential clients. A backend holding a secret is a confidential
// client and may obtain a token for the work a caller cannot authenticate for yet;
// the page it serves may not, whatever precautions the page takes. RFC 10017
// (BCP 212) §6.1 names the arrangement: the backend acts as the confidential client
// on the page's behalf.
//
// **This class issues nothing and knows no SQL.** How a token is obtained is a
// property of the database layer the application runs on, not of this framework,
// so the caller performs the request and reports the outcome back:
//
//     if (token_.needs_refresh()) {
//         token_.begin_refresh();
//         ... issue the request ...
//         // on success: token_.issued(access_token, session, life);
//         // on failure: token_.failed();
//     }
//
// After issued(), take_previous_session() returns the session behind the token that
// was just replaced, if the caller tracks sessions and needs to close it. It is
// handed over only once the replacement is in hand — closing it earlier would
// revoke the token still serving requests.
//
// Threading: single-threaded, like everything on the event loop. needs_refresh() is
// meant to be called from a module's or process's heartbeat().
//
class ServiceToken
{
public:
    ServiceToken() = default;

    /// True while a token is held and the issuer would still accept it. Distinct
    /// from being due for renewal: a token stays usable through the renewal margin,
    /// which is what makes a brief outage survivable.
    bool valid() const noexcept;

    /// The access token, or empty when none is held. Check valid() first.
    const std::string& token() const noexcept { return token_; }

    /// The session behind the current token, if the caller supplied one.
    const std::string& session() const noexcept { return session_; }

    /// Drop the token held. Call when the issuer has rejected it — the next
    /// heartbeat then obtains a fresh one instead of waiting out its nominal life.
    void invalidate() noexcept;

    /// True when the caller should issue a new token now: none is held, the one
    /// held is due for renewal, or an in-flight request has passed its deadline.
    /// Not const — it clears a refresh that never came back, since only a reported
    /// outcome does that otherwise and a request can be cancelled or dropped
    /// without either outcome arriving.
    bool needs_refresh();

    /// Mark a request as in flight and start its deadline.
    void begin_refresh() noexcept;

    /// Report success. @p life is what the issuer said the token is good for;
    /// non-positive means the issuer offered nothing usable and is treated as a
    /// failure. @p session may be empty when the caller tracks none.
    void issued(std::string token, std::string session, std::chrono::seconds life);

    /// Report failure. Backs off, doubling up to the maximum.
    void failed() noexcept;

    /// The session behind the token that issued() replaced, if any; empty
    /// afterwards. Close it only after calling this.
    std::string take_previous_session() noexcept;

    /// The token that issued() replaced, if any; empty afterwards. Needed because
    /// a worker closes a session through daemon.session_close, which names it by
    /// token rather than by code — the api schema its session code would go to is
    /// not open to that role.
    std::string take_previous_token() noexcept;

    /// Whether a request is in flight right now.
    bool refreshing() const noexcept { return refreshing_; }

    static constexpr auto k_renew_margin    = std::chrono::seconds(300);
    static constexpr auto k_min_life        = std::chrono::seconds(30);
    static constexpr auto k_max_life        = std::chrono::seconds(30 * 24 * 3600);
    static constexpr auto k_retry_initial   = std::chrono::seconds(10);
    static constexpr auto k_retry_max       = std::chrono::seconds(1800);
    static constexpr auto k_refresh_timeout = std::chrono::seconds(30);

private:
    std::string token_;
    std::string session_;
    std::string previous_session_;
    std::string previous_token_;

    // Two marks, not one. hard_expiry_ is when the issuer stops accepting the
    // token; renew_at_ is when replacement starts. Collapsing them discards the
    // renewal margin exactly while refreshes are failing, which is when it is
    // wanted.
    std::chrono::steady_clock::time_point hard_expiry_{};
    std::chrono::steady_clock::time_point renew_at_{};
    std::chrono::steady_clock::time_point retry_at_{};
    std::chrono::steady_clock::time_point refresh_deadline_{};

    std::chrono::seconds backoff_{0};
    bool refreshing_{false};
    bool timed_out_{false};
};

} // namespace apostol
