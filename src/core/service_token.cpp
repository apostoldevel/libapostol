#ifdef WITH_POSTGRESQL

#include "apostol/service_token.hpp"
#include "apostol/logger.hpp"
#include "apostol/pg_utils.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

// ─── Construction ────────────────────────────────────────────────────────────

ServiceToken::ServiceToken(PgPool& pool, std::string agent, std::string host)
    : pool_(pool)
    , agent_(std::move(agent))
    , host_(std::move(host))
{}

// ─── valid ───────────────────────────────────────────────────────────────────

bool ServiceToken::valid() const noexcept
{
    return !token_.empty()
        && std::chrono::steady_clock::now() < hard_expiry_;
}

// ─── set_credentials ─────────────────────────────────────────────────────────

void ServiceToken::set_credentials(std::string client_id, std::string client_secret,
                                   std::string scope)
{
    client_id_     = std::move(client_id);
    client_secret_ = std::move(client_secret);
    scope_         = std::move(scope);
}

// ─── invalidate ──────────────────────────────────────────────────────────────

void ServiceToken::invalidate() noexcept
{
    token_.clear();
    session_.clear();
    hard_expiry_ = {};
    renew_at_    = {};
}

// ─── schedule_retry ──────────────────────────────────────────────────────────

void ServiceToken::schedule_retry()
{
    backoff_ = (backoff_.count() == 0)
                   ? k_retry_initial
                   : std::min(backoff_ * 2, k_retry_max);
    retry_at_ = std::chrono::steady_clock::now() + backoff_;
}

// ─── refresh_if_needed ───────────────────────────────────────────────────────

void ServiceToken::refresh_if_needed()
{
    auto now = std::chrono::steady_clock::now();

    // A refresh that never came back must not wedge the token for good: the
    // callbacks are the only thing that clears refreshing_, and a query can be
    // cancelled or dropped without either of them running.
    if (refreshing_) {
        if (now < refresh_deadline_)
            return;
        log_warn("[ServiceToken] refresh for client \"{}\" did not complete in time",
                 client_id_);
        refreshing_ = false;
        schedule_retry();
        return;
    }

    if (!token_.empty() && now < renew_at_)
        return;

    if (now < retry_at_)
        return;

    if (client_id_.empty() || client_secret_.empty())
        return;

    refreshing_      = true;
    refresh_deadline_ = now + k_refresh_timeout;

    nlohmann::json payload{{"grant_type", "client_credentials"}};
    if (!scope_.empty())
        payload["scope"] = scope_;

    auto sql = fmt::format(
        "SELECT * FROM daemon.token({}, {}, {}::jsonb, {}, {})",
        pq_quote_literal(client_id_),
        pq_quote_literal(client_secret_),
        pq_quote_literal(payload.dump()),
        pq_quote_literal(agent_),
        pq_quote_literal(host_));

    // quiet: the statement carries client_secret, and PgPool logs statements.
    pool_.execute(sql,
        [this](std::vector<PgResult> results) {
            refreshing_ = false;

            if (results.empty() || !results[0].ok()
                || results[0].rows() == 0 || results[0].columns() == 0) {
                log_error("[ServiceToken] client \"{}\": no result from daemon.token",
                          client_id_);
                schedule_retry();
                return;
            }

            const char* val = results[0].value(0, 0);
            if (!val || val[0] == '\0') {
                log_error("[ServiceToken] client \"{}\": empty result from daemon.token",
                          client_id_);
                schedule_retry();
                return;
            }

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(val);
            } catch (const std::exception& e) {
                log_error("[ServiceToken] client \"{}\": unparsable result: {}",
                          client_id_, e.what());
                schedule_retry();
                return;
            }

            // daemon.token reports refusals in the body, not as a failed query.
            if (j.contains("error")) {
                auto err = j["error"].is_object() ? j["error"].value("error", "error")
                                                  : j.value("error", "error");
                auto msg = j["error"].is_object() ? j["error"].value("message", "")
                                                  : std::string();
                log_error("[ServiceToken] client \"{}\" refused: {} {}",
                          client_id_, err, msg);
                schedule_retry();
                return;
            }

            if (!j.contains("access_token") || !j["access_token"].is_string()) {
                log_error("[ServiceToken] client \"{}\": response carries no access_token",
                          client_id_);
                schedule_retry();
                return;
            }

            // The session behind the *previous* token, closed only once its
            // replacement is in hand — signing out first would revoke the token we
            // are still serving requests with.
            auto previous_session = session_;

            token_   = j["access_token"].get<std::string>();
            session_ = j.value("session", "");
            backoff_ = std::chrono::seconds(0);

            auto life = k_fallback_life;
            if (j.contains("expires_in") && j["expires_in"].is_number()) {
                auto seconds = static_cast<long long>(j["expires_in"].get<double>());
                if (seconds > 0) {
                    life = std::clamp(std::chrono::seconds(seconds),
                                      k_min_life, k_max_life);
                } else {
                    // A server that says the token is already dead is not offering
                    // one; treating that as an hour would serve a corpse.
                    log_error("[ServiceToken] client \"{}\": expires_in is {}",
                              client_id_, seconds);
                    invalidate();
                    schedule_retry();
                    return;
                }
            }

            auto margin  = std::min(k_renew_margin, life / 2);
            auto now_    = std::chrono::steady_clock::now();
            hard_expiry_ = now_ + life;
            renew_at_    = now_ + (life - margin);

            if (!previous_session.empty()) {
                pool_.execute(
                    fmt::format("SELECT * FROM api.signout({})",
                                pq_quote_literal(previous_session)),
                    [](std::vector<PgResult>) {},
                    [](std::string_view) {},
                    /*quiet=*/true);
            }
        },
        [this](std::string_view error) {
            refreshing_ = false;
            log_error("[ServiceToken] client \"{}\": {}", client_id_, error);
            schedule_retry();
        },
        /*quiet=*/true);
}

// ─── sign_out ────────────────────────────────────────────────────────────────

void ServiceToken::sign_out()
{
    if (session_.empty())
        return;

    auto session = session_;
    invalidate();

    pool_.execute(
        fmt::format("SELECT * FROM api.signout({})", pq_quote_literal(session)),
        [](std::vector<PgResult>) {},
        [](std::string_view) {},
        /*quiet=*/true);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
