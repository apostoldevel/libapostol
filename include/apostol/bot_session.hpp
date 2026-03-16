#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/pg.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace apostol
{

// ─── BotSession ──────────────────────────────────────────────────────────────
//
// Manages an OAuth2 "apibot" session via PostgreSQL.
//
// Mirrors v1 CFileCommon::Authentication() pattern:
//   1. api.login(client_id, secret, agent, host) → token_session
//   2. api.get_session(username, agent, host)     → bot_session
//   3. api.signout(token_session)                 → discard login session
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

    /// Current session string (empty if not authenticated).
    const std::string& session() const noexcept { return session_; }

    /// True if session is non-empty and not expired.
    bool valid() const noexcept;

    /// Set OAuth2 credentials. Must be called before refresh_if_needed().
    void set_credentials(std::string client_id, std::string client_secret,
                         std::string username = "apibot");

    /// Call from heartbeat(). Refreshes session if expired or not yet obtained.
    /// Mirrors v1: login → get_session → signout(login_session).
    void refresh_if_needed();

    /// Sign out the current session. Call from on_stop().
    void sign_out();

    /// Execute api.authorize(session) + api.execute_object_action(id, action).
    /// Calls on_result on success, on_error on failure.
    /// If session is not valid, calls on_error immediately.
    void execute_action(const std::string& id, std::string_view action,
                        PgQuery::ResultHandler    on_result,
                        PgQuery::ExceptionHandler on_error);

private:
    PgPool&     pool_;
    std::string agent_;
    std::string host_;
    std::string client_id_;
    std::string client_secret_;
    std::string username_{"apibot"};
    std::string session_;

    std::chrono::steady_clock::time_point expiry_{};
    std::chrono::steady_clock::time_point retry_at_{};
    bool refreshing_{false};

    static constexpr auto k_refresh_interval = std::chrono::hours(24);
    static constexpr auto k_retry_interval   = std::chrono::seconds(10);
};

} // namespace apostol

#endif // WITH_POSTGRESQL
