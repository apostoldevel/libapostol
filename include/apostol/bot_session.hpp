#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/pg.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── BotSession ──────────────────────────────────────────────────────────────
//
// Holds a service session obtained from db-platform and keeps it fresh.
//
// The class is split along the line between what the framework owns and what it
// borrows. The lifecycle — whether a session is still good, when to renew, how long
// to back off — is transport-agnostic and always built. The three methods that
// speak to db-platform (refresh_if_needed, sign_out, execute_action) exist only
// under WITH_DB_PLATFORM, because the statements they issue are that project's
// contract, not libapostol's:
//
//   1. api.login(client_id, secret, agent, host) → token_session
//   2. api.get_session(username, agent, host)     → bot_session
//   3. api.signout(token_session)                 → discard login session
//
// Build with WITH_DB_PLATFORM=OFF and this integration is absent, along with every
// other reference to that schema in the library.
//
// Usage:
//   BotSession bot(pool, "FileServer/1.0", "localhost");
//   bot.set_credentials("client-id", "client-secret");
//   // In heartbeat:
//   bot.refresh_if_needed();
//   auto session = bot.session();
//
class BotSession
{
public:
    BotSession(PgPool& pool, std::string agent, std::string host);

    /// First session, or empty. Convenience for callers that act on one object and
    /// do not enumerate — the scope is decided by the object, not by the caller.
    ///
    /// Anything that *enumerates* work must iterate sessions() instead: there is one
    /// session per scope, and a caller looking only at the first sees only that
    /// scope's share of the world.
    const std::string& session() const noexcept;

    /// One session per scope, in scope order. Empty when not authenticated.
    const std::vector<std::string>& sessions() const noexcept { return sessions_; }

    /// True if session is non-empty and not expired.
    bool valid() const noexcept;

    /// Set OAuth2 credentials. Must be called before refresh_if_needed().
    void set_credentials(std::string client_id, std::string client_secret,
                         std::string username = "apibot");

#ifdef WITH_DB_PLATFORM

    /// Call from heartbeat(). Refreshes session if expired or not yet obtained.
    /// Mirrors v1: login → get_session → signout(login_session).
    void refresh_if_needed();

    /// Sign out the session obtained by the *login* step. The bot's own sessions are
    /// deliberately left alone: they belong to the donor user, which holds no logout
    /// right, and api.get_sessions hands the same rows back on the next start rather
    /// than making new ones. Attempting to close them only writes "Access denied"
    /// into db.log, once per process per restart, and closes nothing.
    ///
    /// Kept as a no-op so callers need not change; there is nothing left to do here.
    void sign_out();

    /// Execute api.authorize(session) + api.execute_object_action(id, action) under
    /// an explicit session. Use this whenever the object was found by enumerating:
    /// act in the scope you found it in, not in whichever one happens to be first.
    void execute_action(std::string_view session,
                        const std::string& id, std::string_view action,
                        PgQuery::ResultHandler    on_result,
                        PgQuery::ExceptionHandler on_error);

    /// The same under the first session. Only correct when the caller has one scope
    /// or the object is known to live in it — otherwise api.authorize succeeds in
    /// the wrong scope and the action fails on an object it cannot see.
    void execute_action(const std::string& id, std::string_view action,
                        PgQuery::ResultHandler    on_result,
                        PgQuery::ExceptionHandler on_error);

#endif // WITH_DB_PLATFORM

private:
    PgPool&     pool_;
    std::string agent_;
    std::string host_;
    std::string client_id_;
    std::string client_secret_;
    std::string username_{"apibot"};
    std::vector<std::string> sessions_;

    std::chrono::steady_clock::time_point expiry_{};
    std::chrono::steady_clock::time_point retry_at_{};
    bool refreshing_{false};

    static constexpr auto k_refresh_interval = std::chrono::hours(24);
    static constexpr auto k_retry_interval   = std::chrono::seconds(10);
};

} // namespace apostol

#endif // WITH_POSTGRESQL
