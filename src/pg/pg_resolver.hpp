#pragma once

// Internal to libapostol: host name resolution for libpq, off the event loop.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace apostol
{

class EventLoop;

/// libpq resolves the host names of a connection string inside
/// PQconnectStart() and PQresetStart(), on the calling thread — the event
/// loop, for as long as the resolver takes. PgResolver looks the names up on
/// a short-lived thread instead and hands out the same string with `hostaddr`
/// filled in, so libpq connects without looking anything up. `host` stays as
/// written: certificate verification, SNI, the password file and the log
/// still see the name.
///
/// A name with several addresses becomes as many host entries, in the order
/// getaddrinfo() returned them — libpq tries them in that order, as it does
/// with a name. A name that does not resolve is left out; the others are
/// still tried. None left: the lookup failed.
///
/// A string with nothing to look up is handed out unchanged and no thread is
/// started: a numeric address or a socket directory only, `hostaddr` or a
/// service given, host and port lists that do not match (libpq refuses
/// those before any lookup).
///
/// The answer is kept for kMaxAge. An older one is still handed out while a
/// fresh one is looked up; so is the last good one when a fresh lookup
/// fails — a resolver outage does not stop connections to a server that is
/// up. stale() — called after a connect failed — asks for a fresh lookup.
class PgResolver
{
public:
    struct Outcome
    {
        bool        ok   = true;   ///< addresses for at least one host
        bool        kept = false;  ///< failed, the previous answer still in use
        std::string error;         ///< the names that did not resolve, and why
    };
    using ReadyHandler = std::function<void(const Outcome&)>;

    static constexpr std::chrono::seconds kMaxAge{60};

    /// @param on_ready  called from the loop when a lookup ends — the first
    ///                  one, which conninfo() made the owner wait for, and
    ///                  every refresh. Must not throw.
    PgResolver(EventLoop& loop, std::string conninfo, ReadyHandler on_ready);
    ~PgResolver();

    PgResolver(const PgResolver&)            = delete;
    PgResolver& operator=(const PgResolver&) = delete;

    /// The string to connect with now; nullopt while the first answer is
    /// awaited (on_ready comes with it). Starts a lookup when one is due.
    std::optional<std::string> conninfo();

    /// A connect failed: the next conninfo() looks the names up again.
    void stale() noexcept { stale_ = true; }

    /// A lookup is under way.
    bool pending() const noexcept { return job_ != nullptr; }

    /// The string has names to look up.
    bool resolves() const noexcept { return resolves_; }

    /// The parsed string: the host entries, their kinds, the names to look up.
    struct Plan;

private:
    struct Job;

    void start();
    void on_done() noexcept;

    EventLoop&   loop_;
    std::string  conninfo_;
    ReadyHandler on_ready_;

    std::unique_ptr<Plan> plan_;      // what the string asks for; null — nothing to look up
    bool                  resolves_ = false;

    std::optional<std::string>            resolved_;
    std::chrono::steady_clock::time_point resolved_at_{};
    bool                                  stale_ = false;

    std::shared_ptr<Job> job_;        // the lookup under way; its thread holds it too
};

} // namespace apostol
