#pragma once

#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

#include "apostol/logger.hpp"
#include "apostol/pg.hpp"
#include "apostol/service_token.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace apostol::db_platform
{

// ─── db-platform integration ─────────────────────────────────────────────────
//
// The statements libapostol issues against db-platform, gathered in one place so
// that the framework's own code stays free of them and the modules and processes
// that need them do not each carry a copy.
//
// Everything here names a function of that project's PL/pgSQL API. Nothing in
// libapostol outside this layer does.

/// Ask daemon.token for an access token with the client credentials grant and
/// report the outcome into @p token. A no-op unless token.needs_refresh(), so it
/// is safe — and meant — to call every heartbeat.
///
/// The statement carries @p client_secret and is executed quiet; failures are
/// logged through @p log, prefixed with @p tag, because otherwise a missing or
/// wrong credential is an invisible retry loop.
///
/// On success the session behind the *previous* token is signed out, once its
/// replacement is in hand.
void refresh_service_token(PgPool& pool, ServiceToken& token, Logger& log,
                           std::string_view tag,
                           std::string client_id, std::string client_secret,
                           std::string scope,
                           std::string agent, std::string host);

/// api.signout(session) — closing a session by its code.
///
/// Only for a pool whose role has USAGE on the api schema. The worker role (daemon)
/// does not: it reaches daemon.* and nothing else, so a worker calling this gets
/// "permission denied for schema api" no matter what privileges it holds on the
/// function itself. Use close_session() there.
///
/// Nothing can be done with the result at the point this is called — the process is
/// usually on its way out — but it must not be discarded either: api.signout returns
/// false when SignOut refused, and a refusal that nobody notices leaves the session
/// row behind for good. Pass a logger and the refusal is at least visible.
void sign_out(PgPool& pool, std::string_view session,
              Logger* log = nullptr, std::string_view tag = {});

/// daemon.session_close(token) — closing a session by the access token that names
/// it. The same work as sign_out, reached through the daemon schema, which is what
/// a worker's role can see.
///
/// This is the asymmetry that made service sessions immortal: the token was
/// obtained through daemon.token and released through api.signout, and only the
/// first of those two schemas is open to the role doing the work.
///
/// Fire and forget: for a caller that has nobody to answer — a process on its way
/// out. Refusals and failures are logged through @p log; a session that is already
/// gone is not, because at shutdown that is the common case.
void close_session(PgPool& pool, std::string_view token,
                   Logger* log = nullptr, std::string_view tag = {});

/// What daemon.session_close answered.
struct SessionCloseResult
{
    enum class Status
    {
        closed,   ///< the session the token names is closed now
        gone,     ///< the token names no live session: expired, swept, closed elsewhere
        refused,  ///< the database refused — the token is not ours, access denied, …
        failed,   ///< no answer to read: the pool, the connection, a malformed reply
    };

    Status      status = Status::failed;
    std::string message;   ///< the database's or the pool's words, for a log line
};

using SessionCloseHandler = std::function<void(const SessionCloseResult&)>;

/// The same statement for a caller that must answer someone — an endpoint closing
/// the session it was asked to. @p on_done is called exactly once, from the pool's
/// callback; with an empty @p token there is nothing to run and it is called at
/// once, before this returns, with Status::gone.
///
/// Only the access token is accepted. daemon.session_close validates it as a JWT,
/// so a refresh token is refused, and an access token past its lifetime reads as
/// gone even while its session and refresh token live on: to close such a session,
/// renew the pair first (daemon.refresh_token) and close by the new access token.
void close_session(PgPool& pool, std::string_view token, SessionCloseHandler on_done);

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
