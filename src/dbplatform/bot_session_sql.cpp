#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

// The db-platform half of BotSession: the statements themselves.
//
// Kept apart from src/core/bot_session.cpp on purpose. That file holds the session
// lifecycle, which is the framework's own business; this one names api.login,
// api.get_session, api.signout, api.authorize and api.execute_object_action, which
// are db-platform's. Building with WITH_DB_PLATFORM=OFF leaves the library with no
// reference to that schema at all.

#include "apostol/bot_session.hpp"
#include "apostol/db_platform.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

// ─── refresh_if_needed ───────────────────────────────────────────────────────
//
// Mirrors v1 CFileCommon::Authentication():
//   1. api.login(client_id, secret, agent, host) → token_session
//   2. api.get_session(username, agent, host)     → bot_session
//   3. api.signout(token_session)                 → discard login session

void BotSession::refresh_if_needed()
{
    if (valid() || refreshing_)
        return;

    // Throttle retries on error
    auto now = std::chrono::steady_clock::now();
    if (now < retry_at_)
        return;

    if (client_id_.empty() || client_secret_.empty())
        return;

    refreshing_ = true;

    // Steps 1+2 in one SQL batch (same connection):
    //   api.login()        → establishes session context
    //   api.get_sessions() → the donor's session in every scope
    //
    // Plural, deliberately. api.get_sessions walks db.scope and answers one session
    // per scope; api.get_session answers one, whichever scope the profile resolves
    // to. A process that enumerates work — jobs, reports, an outbox — sees only that
    // one scope's share if it asks for the singular, and in a multi-scope
    // installation the rest is simply never done.
    auto sql = fmt::format(
        "SELECT * FROM api.login({}, {}, {}, {});\n"
        "SELECT * FROM api.get_sessions({}, {}, {})",
        pq_quote_literal(client_id_),
        pq_quote_literal(client_secret_),
        pq_quote_literal(agent_),
        pq_quote_literal(host_),
        pq_quote_literal(username_),
        pq_quote_literal(agent_),
        pq_quote_literal(host_));

    // quiet: the statement carries client_secret, and PgPool logs statement text.
    pool_.execute(sql,
        [this](std::vector<PgResult> results) {
            // results[0] = login, results[1] = get_session
            if (results.size() < 2) {
                refreshing_ = false;
                retry_at_ = std::chrono::steady_clock::now() + k_retry_interval;
                return;
            }

            // Extract token_session from login result (for signout)
            std::string token_session;
            if (results[0].ok() && results[0].rows() > 0 && results[0].columns() > 0) {
                const char* val = results[0].value(0, 0);
                if (val && val[0] != '\0') {
                    try {
                        auto j = nlohmann::json::parse(val);
                        if (j.contains("session"))
                            token_session = j["session"].get<std::string>();
                        else if (j.contains("token"))
                            token_session = j["token"].get<std::string>();
                    } catch (...) {
                        token_session = val;
                    }
                }
            }

            // Extract the bot sessions — one row per scope.
            if (results[1].ok() && results[1].columns() > 0) {
                std::vector<std::string> found;

                for (int row = 0; row < results[1].rows(); ++row) {
                    const char* v = results[1].value(row, 0);
                    if (!v || v[0] == '\0')
                        continue;

                    try {
                        auto j = nlohmann::json::parse(v);
                        found.push_back(j.contains("session")
                                            ? j["session"].get<std::string>()
                                            : std::string(v));
                    } catch (...) {
                        found.emplace_back(v);
                    }
                }

                if (!found.empty()) {
                    sessions_ = std::move(found);
                    expiry_ = std::chrono::steady_clock::now() + k_refresh_interval;
                }
            }

            // Step 3: signout the login token_session (cleanup)
            if (!token_session.empty()) {
                auto sql3 = fmt::format(
                    "SELECT * FROM api.signout({})",
                    pq_quote_literal(token_session));

                pool_.execute(sql3,
                    [this](std::vector<PgResult> /*r*/) {
                        refreshing_ = false;
                    },
                    [this](std::string_view /*error*/) {
                        refreshing_ = false;
                    },
                    /*quiet=*/true);
            } else {
                refreshing_ = false;
                retry_at_ = std::chrono::steady_clock::now() + k_retry_interval;
            }
        },
        [this](std::string_view /*error*/) {
            refreshing_ = false;
            retry_at_ = std::chrono::steady_clock::now() + k_retry_interval;
        },
        /*quiet=*/true);
}

// ─── execute_action ──────────────────────────────────────────────────────────

void BotSession::execute_action(const std::string& id, std::string_view action,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_error)
{
    execute_action(session(), id, action, std::move(on_result), std::move(on_error));
}

void BotSession::execute_action(std::string_view session,
                                const std::string& id, std::string_view action,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_error)
{
    if (session.empty() || !valid()) {
        if (on_error)
            on_error("BotSession: not authenticated");
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_object_action({}::uuid, {})",
        pq_quote_literal(session),
        pq_quote_literal(id),
        pq_quote_literal(action));

    // quiet: the statement carries the bot's session code.
    pool_.execute(sql, std::move(on_result), std::move(on_error), /*quiet=*/true);
}

// ─── sign_out ────────────────────────────────────────────────────────────────

void BotSession::sign_out()
{
    // Nothing to do, and that is the point.
    //
    // The bot's sessions belong to the donor user (api.get_sessions leaves suid
    // equal to userid), and that user holds no logout right — deliberately: it is
    // a donor of the system session, never a party to a login. SessionOut checks
    // the logout bit of the session's current user and refuses, then records
    // "Access denied" under code 9001. Ten of those per stack restart, in a log
    // kept for ten years.
    //
    // Nor is there anything to reclaim: api.get_sessions matches on
    // (suid, scope, agent) and hands the same rows back on the next start. The
    // session outliving the process is how this has always worked — the v1
    // processes closed only the login session and left these alone.
    //
    // The login session *is* closed, in refresh_if_needed, by the client that
    // opened it and may close it.
    sessions_.clear();
    expiry_ = {};
}

} // namespace apostol

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
