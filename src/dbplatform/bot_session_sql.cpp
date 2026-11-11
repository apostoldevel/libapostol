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
#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace apostol
{

// ─── detail::SignInReport ────────────────────────────────────────────────────

namespace detail
{

SignInReport::Line SignInReport::failed(const std::string& why,
                                        std::chrono::steady_clock::time_point now)
{
    ++failures_;

    const bool written = std::find(written_.begin(), written_.end(), why) != written_.end();
    if (!written && written_.size() < k_reasons) {
        written_.push_back(why);
        last_line_ = now;
        return Line::reason;
    }

    // Written already, or too many distinct reasons to keep apart: a reminder
    // once per interval, counted from the last line of either kind.
    if (now - last_line_ >= k_interval) {
        last_line_ = now;
        return Line::reminder;
    }

    return Line::none;
}

bool SignInReport::obtained() noexcept
{
    const bool ended = failures_ > 0;
    reset();
    return ended;
}

void SignInReport::reset() noexcept
{
    written_.clear();
    last_line_ = {};
    failures_  = 0;
}

} // namespace detail

// ─── failed / obtained ───────────────────────────────────────────────────────
//
// A background component calls refresh_if_needed() from its heartbeat and, while
// valid() is false, simply takes no work. Nothing else in the process says why: the
// pool writes the database's error to its own statement log, without naming who
// asked. These two write it where an operator looks, under the component's agent.
//
// The wording says "failed", not "refused": the same path carries a credential
// refusal, a connection lost under the batch and a batch never sent.

void BotSession::failed(std::string why)
{
    last_error_ = std::move(why);

    const auto line = report_.failed(last_error_, std::chrono::steady_clock::now());
    if (line == detail::SignInReport::Line::none)
        return;

    Logger* log = EventLoop::diagnostics();
    if (log == nullptr)
        return;

    try {
        if (line == detail::SignInReport::Line::reason)
            log->error("BotSession [{}]: no session — sign-in as client '{}' failed: {}; "
                       "retried every {} s", agent_, client_id_, last_error_,
                       k_retry_interval.count());
        else
            log->error("BotSession [{}]: still no session — {} failed sign-in attempts as "
                       "client '{}', the last: {}", agent_, report_.failures(), client_id_,
                       last_error_);
    } catch (...) {}
}

void BotSession::obtained()
{
    const std::size_t failures = report_.failures();
    last_error_.clear();

    if (!report_.obtained())
        return;

    if (Logger* log = EventLoop::diagnostics()) {
        try {
            log->notice("BotSession [{}]: session obtained after {} failed sign-in attempt(s)",
                        agent_, failures);
        } catch (...) {}
    }
}

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

    if (client_id_.empty() || client_secret_.empty()) {
        retry_at_ = now + k_retry_interval;
        failed(client_id_.empty() ? "no client credentials set" : "no client secret set");
        return;
    }

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
                failed(fmt::format("the sign-in batch answered with {} result(s), expected 2",
                                   results.size()));
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
                    obtained();
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
        [this](std::string_view error) {
            refreshing_ = false;
            retry_at_ = std::chrono::steady_clock::now() + k_retry_interval;
            // The batch is quiet: the pool hands over text that does not quote the
            // statement, which carries the secret.
            failed(std::string(error));
        },
        /*quiet=*/true);
}

// ─── execute_action ──────────────────────────────────────────────────────────

void BotSession::execute_action(const std::string& id, std::string_view action,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_error,
                                PgRetry                   retry)
{
    execute_action(session(), id, action, std::move(on_result), std::move(on_error), retry);
}

void BotSession::execute_action(std::string_view session,
                                const std::string& id, std::string_view action,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_error,
                                PgRetry                   retry)
{
    // Two refusals, two texts — deliberately, and in this order.
    //
    // With one shared text ("not authenticated") a line such as
    //
    //   TaskScheduler: action 'cancel' failed for <uuid>: BotSession: not authenticated
    //
    // points at OAuth2 and the apibot account even when the bot holds a session
    // and the caller handed over an empty one, for an object it no longer tracks.
    // Each refusal therefore names its own layer.
    //
    // That combination (a valid bot session, an empty one passed in) is only one
    // way to get here. Some callers keep no valid() gate of
    // their own (MessageServer::do_send, ReportServer::do_complete and do_abort), and
    // a gate that is kept may have been passed before an asynchronous callback rather
    // than inside one, in which case it is stale by the time the call lands. The first
    // branch below is genuinely reachable — a session does expire — so its text has to
    // stand on its own, not merely be the one nobody reaches.
    //
    // The object's own state is tested first on purpose. Whenever the object holds no
    // session at all, session() is empty and valid() is false together, so the
    // convenience overload above — which delegates with session() — would, under the
    // opposite order, blame the caller for the object's own missing session, every
    // time. That is the same misdirection, mirrored.
    //
    // Neither implication is an equivalence, and one exception matters here: a session
    // string that is itself empty leaves sessions_ non-empty, so valid() stays true
    // while session() is empty, and the second branch does fire for what the bot
    // produced. Its root is the emptiness filter in refresh_if_needed, which tests the
    // raw value before the JSON is parsed — not this order of tests.
    //
    // Behaviour is unchanged either way: both branches refuse before the database is
    // touched.
    if (!valid()) {
        if (on_error)
            on_error("BotSession: not authenticated:"
                     " no session obtained, or it expired");
        return;
    }

    if (session.empty()) {
        if (on_error)
            on_error("BotSession: empty session passed by the caller"
                     " (not an authentication failure)");
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_object_action({}::uuid, {})",
        pq_quote_literal(session),
        pq_quote_literal(id),
        pq_quote_literal(action));

    // quiet: the statement carries the bot's session code.
    pool_.execute(sql, std::move(on_result), std::move(on_error), /*quiet=*/true, retry);
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
    // "Access denied" under code 9001 — one refusal per session, on every
    // process stop, kept in the event log for good.
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
