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

    // api.authorize first, the way execute_action does it. SessionOut runs
    // DoLogout and touches db.profile through the session context; a project is
    // free to put real work in DoLogout, and a bare api.signout would run it with
    // no session established.
    pool.execute(fmt::format("SELECT * FROM api.authorize({});\n"
                             "SELECT * FROM api.signout({})",
                             pq_quote_literal(session),
                             pq_quote_literal(session)),
        [log, label](std::vector<PgResult> results) {
            if (!log)
                return;

            // results[0] = api.authorize, results[1] = api.signout
            if (results.size() < 2 || !results[1].ok()) {
                log->warn("{} sign out: no result", label);
                return;
            }

            // api.signout returns boolean. False means SignOut refused — most
            // often the ACL check inside SessionOut — and the row stays.
            if (results[1].rows() > 0 && results[1].columns() > 0) {
                const char* v = results[1].value(0, 0);
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
            sign_out(pool, token.take_previous_session());
        },
        [&token, &log, label, client_id](std::string_view error) {
            log.error("{} service token for \"{}\": {}", label, client_id, error);
            token.failed();
        },
        /*quiet=*/true);
}

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
