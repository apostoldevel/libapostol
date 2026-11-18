#include "apostol/service_token.hpp"

#include <algorithm>

namespace apostol
{

// ─── valid ───────────────────────────────────────────────────────────────────

bool ServiceToken::valid() const noexcept
{
    return !token_.empty()
        && std::chrono::steady_clock::now() < hard_expiry_;
}

// ─── invalidate ──────────────────────────────────────────────────────────────

void ServiceToken::invalidate() noexcept
{
    token_.clear();
    session_.clear();
    hard_expiry_ = {};
    renew_at_    = {};
}

// ─── needs_refresh ───────────────────────────────────────────────────────────

bool ServiceToken::needs_refresh()
{
    auto now = std::chrono::steady_clock::now();

    // A refresh that never came back must not wedge the token for good: only a
    // reported outcome clears refreshing_, and a request can be cancelled or
    // dropped without either outcome arriving.
    if (refreshing_) {
        if (now < refresh_deadline_)
            return false;
        refreshing_ = false;
        timed_out_  = true;
        failed();
        return false;
    }

    if (!token_.empty() && now < renew_at_)
        return false;

    if (now < retry_at_)
        return false;

    return true;
}

// ─── begin_refresh ───────────────────────────────────────────────────────────

void ServiceToken::begin_refresh() noexcept
{
    refreshing_       = true;
    timed_out_        = false;
    refresh_deadline_ = std::chrono::steady_clock::now() + k_refresh_timeout;
}

// ─── issued ──────────────────────────────────────────────────────────────────

void ServiceToken::issued(std::string token, std::string session,
                          std::chrono::seconds life)
{
    refreshing_ = false;

    // An issuer saying the token is already dead is not offering one; treating
    // that as a default lifetime would serve a corpse.
    if (token.empty() || life.count() <= 0) {
        invalidate();
        failed();
        return;
    }

    life = std::clamp(life, k_min_life, k_max_life);

    previous_session_ = std::move(session_);
    previous_token_   = std::move(token_);

    token_   = std::move(token);
    session_ = std::move(session);
    backoff_ = std::chrono::seconds(0);
    retry_at_ = {};

    auto margin  = std::min(k_renew_margin, life / 2);
    auto now     = std::chrono::steady_clock::now();
    hard_expiry_ = now + life;
    renew_at_    = now + (life - margin);
}

// ─── failed ──────────────────────────────────────────────────────────────────

void ServiceToken::failed() noexcept
{
    refreshing_ = false;
    const auto now  = std::chrono::steady_clock::now();
    const bool held = !token_.empty() && now < hard_expiry_;

    // While a token is held, a long delay costs nothing: the token serves. Once
    // none is, every caller is refused until the next attempt, so the delay is
    // kept short — and the attempt that would land past the token's expiry is
    // brought forward to it, so the outage does not outlast the issuer's.
    const auto cap = held ? k_retry_max : k_retry_max_unheld;
    backoff_ = (backoff_.count() == 0)
                   ? k_retry_initial
                   : std::min(backoff_ * 2, cap);
    retry_at_ = now + backoff_;
    if (held && retry_at_ > hard_expiry_)
        retry_at_ = hard_expiry_;
}

// ─── take_previous_session ───────────────────────────────────────────────────

std::string ServiceToken::take_previous_session() noexcept
{
    return std::move(previous_session_);
}

// ─── take_previous_token ─────────────────────────────────────────────────────

std::string ServiceToken::take_previous_token() noexcept
{
    return std::move(previous_token_);
}

} // namespace apostol
