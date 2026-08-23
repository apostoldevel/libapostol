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

void close_session(PgPool& pool, std::string_view token, Logger* log, std::string_view tag)
{
    if (token.empty())
        return;

    std::string label(tag);

    // daemon.session_close validates the token, takes the session code from its
    // "sub" claim and calls SessionOut. Reported failures come back as a json
    // object with an "error" member rather than as a failed statement.
    pool.execute(fmt::format("SELECT * FROM daemon.session_close({})",
                             pq_quote_literal(token)),
        [log, label](std::vector<PgResult> results) {
            if (!log)
                return;

            if (results.empty() || !results[0].ok()) {
                log->warn("{} close session failed: {}", label,
                          results.empty() ? "no result" : results[0].error_message());
                return;
            }

            if (results[0].rows() == 0 || results[0].columns() == 0)
                return;

            const char* v = results[0].value(0, 0);
            if (!v)
                return;

            try {
                auto j = nlohmann::json::parse(v);
                if (!j.contains("error"))
                    return;

                const auto& e = j["error"];
                const int code = e.is_object() ? e.value("code", 0) : 0;

                // "Token not FOUND or has expired". At shutdown this is not a
                // refusal but a job already done: the session went with an expired
                // token, a sweep, or a sign-out elsewhere. Warning about it would
                // put a line per worker per restart into the log and teach whoever
                // reads it to ignore the ones that matter.
                //
                // Both numbers, and not out of caution. db-platform 1.2.13 moved
                // this from ERR-403-001 to ERR-401-008, because RFC 6750 §3.1 puts
                // an expired token at 401. The status travels inside the identifier,
                // so correcting it meant a new one — and a binary from master will
                // meet databases on either side of that patch for as long as the
                // rollout takes. Accepting only the new number would bring the noise
                // back everywhere the patch has not landed yet.
                //
                // The error object daemon.* returns carries a status and a
                // message, and no identifier — api.* does return one, but this path
                // does not go through api.* — so this cannot be narrowed to the one
                // code: 401 here also covers the rest of that group. On the way in
                // to daemon.session_close only TokenExpired answers 401 at all; the
                // rest of that path (IssuerNotFound, AudienceNotFound, TokenError,
                // TokenBelong, AccessDenied) is group 400. At a shutdown close the
                // cost of being wrong is a warning not written, which is the same
                // thing this branch is for.
                //
                // The 403 half stops being needed once every database is on 1.2.13
                // or later; until someone can say that, it stays.
                if (code == 401 || code == 403)
                    return;

                log->warn("{} close session refused: {}", label,
                          e.is_object() ? e.value("message", "") : std::string(v));
            } catch (...) {
                // Not json: the claims came back as something else. Nothing to say.
            }
        },
        [log, label](std::string_view error) {
            if (log)
                log->warn("{} close session failed: {}", label, error);
        },
        /*quiet=*/true);
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
