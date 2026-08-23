#pragma once

#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

#include "apostol/logger.hpp"
#include "apostol/pg.hpp"
#include "apostol/service_token.hpp"

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

/// api.signout(session).
///
/// Nothing can be done with the result at the point this is called — the process is
/// usually on its way out — but it must not be discarded either: api.signout returns
/// false when SignOut refused, and a refusal that nobody notices leaves the session
/// row behind for good. Pass a logger and the refusal is at least visible.
void sign_out(PgPool& pool, std::string_view session,
              Logger* log = nullptr, std::string_view tag = {});

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
