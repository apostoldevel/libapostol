#ifdef WITH_POSTGRESQL

#include "apostol/bot_session.hpp"

namespace apostol
{

// ─── Construction ────────────────────────────────────────────────────────────

BotSession::BotSession(PgPool& pool, std::string agent, std::string host)
    : pool_(pool)
    , agent_(std::move(agent))
    , host_(std::move(host))
{}

// ─── valid ───────────────────────────────────────────────────────────────────

bool BotSession::valid() const noexcept
{
    return !sessions_.empty()
        && std::chrono::steady_clock::now() < expiry_;
}

const std::string& BotSession::session() const noexcept
{
    static const std::string empty;
    return sessions_.empty() ? empty : sessions_.front();
}

// ─── set_credentials ─────────────────────────────────────────────────────────

void BotSession::set_credentials(std::string client_id, std::string client_secret,
                                 std::string username)
{
    client_id_     = std::move(client_id);
    client_secret_ = std::move(client_secret);
    username_      = std::move(username);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
