#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

#include "apostol/db_platform.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol::db_platform
{

// ─── sign_out ────────────────────────────────────────────────────────────────

void sign_out(PgPool& pool, std::string_view session, Logger* log, std::string_view tag)
{
    if (session.empty())
        return;

    std::string label(tag);

    pool.execute(fmt::format("SELECT * FROM api.signout({})",
                             pq_quote_literal(session)),
        [log, label](std::vector<PgResult> results) {
            if (!log)
                return;

            if (results.empty() || !results[0].ok()) {
                log->warn("{} sign out failed: {}", label,
                          results.empty() ? "no result" : results[0].error_message());
                return;
            }

            // api.signout returns boolean. False means SignOut refused — the ACL
            // check inside SessionOut is one way — and the row stays.
            if (results[0].rows() > 0 && results[0].columns() > 0) {
                const char* v = results[0].value(0, 0);
                if (v && (v[0] == 'f' || v[0] == 'F'))
                    log->warn("{} sign out refused; the session row remains. "
                              "Look for code 9001 in db.log", label);
            }
        },
        [log, label](std::string_view error) {
            if (log)
                log->warn("{} sign out failed: {}", label, error);
        },
        /*quiet=*/true);
}

// ─── close_session ───────────────────────────────────────────────────────────

namespace
{

using CloseStatus = SessionCloseResult::Status;

// One reading of daemon.session_close's answer for both overloads. It reports
// refusals as a json object with an "error" member rather than as a failed
// statement, and on success returns the claims of the token it closed by.
SessionCloseResult read_session_close(const std::vector<PgResult>& results)
{
    if (results.empty())
        return {CloseStatus::failed, "no result"};

    if (!results[0].ok()) {
        const char* msg = results[0].error_message();
        return {CloseStatus::failed, msg ? msg : "unknown error"};
    }

    if (results[0].rows() == 0 || results[0].columns() == 0)
        return {CloseStatus::failed, "no row"};

    const char* v = results[0].value(0, 0);
    if (!v)
        return {CloseStatus::failed, "null answer"};

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(v);
    } catch (...) {
        return {CloseStatus::failed, "the answer is not json"};
    }

    if (!j.is_object() || !j.contains("error"))
        return {CloseStatus::closed, {}};

    const auto& e = j["error"];

    int code = 0;
    std::string error_id;
    std::string message;
    try {
        code = e.is_object() ? e.value("code", 0) : 0;

        if (e.is_object() && e.contains("error") && e["error"].is_string()) {
            auto value = e["error"].get<std::string>();
            if (value.rfind("ERR-", 0) == 0)
                error_id = std::move(value);
        }

        message = e.is_object() ? e.value("message", "") : std::string(v);
    } catch (const nlohmann::json::exception&) {
        // A member of the wrong type — a null message, a code in quotes. The
        // answer is unreadable, not a refusal.
        return {CloseStatus::failed, v};
    }

    // Not a refusal but the database failing: daemon.session_close catches every
    // exception, and one without an ERR- identifier — a lock timeout, a
    // serialisation failure, a broken constraint inside SessionOut — comes back
    // as code 500. Read as a refusal it would tell a caller the session is closed
    // while it lives on; RFC 7009 §2.2.1 asks for 503 there, not 200.
    if (code >= 500)
        return {CloseStatus::failed, std::move(message)};

    // "Token not FOUND or has expired": the session is already gone — with an
    // expired token, a sweep, or a sign-out elsewhere. For a caller closing it that
    // is a job done, not a refusal.
    //
    // Three ways to recognise it, one per era of the database this binary may be
    // talking to, and a rollout puts all three in front of it at once:
    //
    //   1.2.14 and later — daemon.* reports the identifier, so the branch is exact
    //     and nothing else is taken for it;
    //   1.2.13 — the status is right and the identifier absent;
    //   before 1.2.13 — TokenExpired was still ERR-403-001.
    //
    // Falling back to the status takes the rest of its group too. On the way in to
    // daemon.session_close only TokenExpired answers 401 at all — IssuerNotFound,
    // AudienceNotFound, TokenError, TokenBelong and AccessDenied are all group 400 —
    // so in practice it takes nothing else.
    const bool gone = !error_id.empty()
        ? (error_id == "ERR-401-008" || error_id == "ERR-403-001")
        : (code == 401 || code == 403);

    return {gone ? CloseStatus::gone : CloseStatus::refused, std::move(message)};
}

} // namespace

void close_session(PgPool& pool, std::string_view token, SessionCloseHandler on_done)
{
    if (token.empty()) {
        if (on_done)
            on_done({CloseStatus::gone, {}});
        return;
    }

    // quiet: the statement carries the access token.
    pool.execute(fmt::format("SELECT * FROM daemon.session_close({})",
                             pq_quote_literal(token)),
        [on_done](std::vector<PgResult> results) {
            if (on_done)
                on_done(read_session_close(results));
        },
        [on_done](std::string_view error) {
            if (on_done)
                on_done({CloseStatus::failed, std::string(error)});
        },
        /*quiet=*/true);
}

void close_session(PgPool& pool, std::string_view token, Logger* log, std::string_view tag)
{
    if (token.empty() || !log) {
        close_session(pool, token, SessionCloseHandler{});
        return;
    }

    // "Gone" is not worth a line here. This overload is what a process calls at
    // shutdown, and there a session that went on its own is the common case: a
    // warning per worker per restart would teach whoever reads the log to ignore
    // the ones that matter.
    close_session(pool, token, [log, label = std::string(tag)](const SessionCloseResult& r) {
        switch (r.status) {
            case CloseStatus::closed:
            case CloseStatus::gone:
                return;
            case CloseStatus::refused:
                log->warn("{} close session refused: {}", label, r.message);
                return;
            case CloseStatus::failed:
                log->warn("{} close session failed: {}", label, r.message);
                return;
        }
    });
}

// ─── refresh_service_token ───────────────────────────────────────────────────

void refresh_service_token(PgPool& pool, ServiceToken& token, Logger& log,
                           std::string_view tag,
                           std::string client_id, std::string client_secret,
                           std::string scope,
                           std::string agent, std::string host)
{
    if (!token.needs_refresh())
        return;

    if (client_id.empty() || client_secret.empty()) {
        log.error("{} service token: no client id or secret configured", tag);
        token.failed();
        return;
    }

    token.begin_refresh();

    nlohmann::json payload{{"grant_type", "client_credentials"}};
    if (!scope.empty())
        payload["scope"] = scope;

    auto sql = fmt::format(
        "SELECT * FROM daemon.token({}, {}, {}::jsonb, {}, {})",
        pq_quote_literal(client_id),
        pq_quote_literal(client_secret),
        pq_quote_literal(payload.dump()),
        pq_quote_literal(agent),
        pq_quote_literal(host));

    std::string label(tag);

    // quiet: the statement carries client_secret, and PgPool logs statement text.
    pool.execute(sql,
        [&pool, &token, &log, label, client_id](std::vector<PgResult> results) {
            if (results.empty() || !results[0].ok()
                || results[0].rows() == 0 || results[0].columns() == 0) {
                log.error("{} service token for \"{}\": no result from daemon.token",
                          label, client_id);
                token.failed();
                return;
            }

            const char* val = results[0].value(0, 0);
            if (!val || val[0] == '\0') {
                log.error("{} service token for \"{}\": empty result", label, client_id);
                token.failed();
                return;
            }

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(val);
            } catch (const std::exception& e) {
                log.error("{} service token for \"{}\": unparsable result: {}",
                          label, client_id, e.what());
                token.failed();
                return;
            }

            // daemon.token reports refusals in the body, not as a failed query.
            if (j.contains("error")) {
                const auto& e = j["error"];
                log.error("{} service token for \"{}\" refused: {} {}", label, client_id,
                          e.is_object() ? e.value("error", "error") : std::string("error"),
                          e.is_object() ? e.value("message", "") : std::string());
                token.failed();
                return;
            }

            std::chrono::seconds life{3600};
            if (j.contains("expires_in") && j["expires_in"].is_number())
                life = std::chrono::seconds(
                    static_cast<long long>(j["expires_in"].get<double>()));

            token.issued(j.value("access_token", ""), j.value("session", ""), life);

            if (!token.valid()) {
                log.error("{} service token for \"{}\": response carried no usable token",
                          label, client_id);
                return;
            }

            // The session behind the token just replaced — closed only now, because
            // closing it earlier would revoke the token still serving requests.
            // By token, not by session code: this runs on the worker's pool, whose
            // role cannot reach the api schema.
            close_session(pool, token.take_previous_token(), &log, label);
            token.take_previous_session();   // discard; closed above
        },
        [&token, &log, label, client_id](std::string_view error) {
            log.error("{} service token for \"{}\": {}", label, client_id, error);
            token.failed();
        },
        /*quiet=*/true);
}

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
