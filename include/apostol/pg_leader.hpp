#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/event_loop.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

class Logger;
class PgConnection;
class PgResult;

// ─── PgLeaderLock ────────────────────────────────────────────────────────────
//
// "One active copy per role" across processes, containers and hosts that
// share a database: a session-level pg_try_advisory_lock held on a connection
// of its own. Whoever holds the lock is the leader; everyone else is a
// standby and asks again every `interval`.
//
// The connection is DEDICATED — not taken from a PgPool. A pool reconnects on
// its own, and a session lock does not survive a reconnect: the pool would
// come back healthy while the lock had silently gone to someone else.
//
// It must reach PostgreSQL in session mode — directly, or through a pgbouncer
// pool in `session` mode. Under transaction pooling a session lock means
// nothing. The probe below reports the lock lost when it lands on a server
// connection that does not hold it — a chance, not a guard.
//
// Losing the lock:
//   - the connection breaks (EOF, error, EPOLLHUP);
//   - the server does not answer within answer_timeout();
//   - the probe finds the lock no longer held by this session;
//   - fresh() finds the socket closed by the server (on_lost runs from
//     inside that fresh() call).
// on_lost is called once, and the object goes inert: it does not try to take
// the lock again, and it keeps the connection — a slow session still holds
// the lock — until it is destroyed, after the caller has stopped its work. Leadership is re-acquired by a fresh start — in
// libapostol, the master respawns the process, and the new one begins as a
// standby. Modules are not asked to survive on_stop() → on_start().
//
// Server-side bound: the session sets idle_session_timeout (PostgreSQL 14+)
// to server_timeout(), longer than answer_timeout() + interval. A leader that
// vanished without closing its socket (a host lost, a partition) would
// otherwise hold the lock until TCP keepalive notices — two hours by default.
// The ordering matters: the old leader steps down on its own clock BEFORE
// the server frees the lock for the next one.
//
// The session's application_name is the holder string, so the current leader
// is visible in pg_stat_activity, and a standby logs who holds the lock.
//
// Handlers must not destroy the object. Destroying it closes the connection,
// which releases the lock; the EventLoop must still be alive then.
//
class PgLeaderLock
{
public:
    using AcquiredHandler = std::function<void()>;
    using LostHandler     = std::function<void(std::string_view why)>;

    /// @p key     — the role, e.g. "csms/helper"; hashed to the lock id.
    /// @p holder  — who asks, e.g. "csms leader helper node-a:123";
    ///              becomes application_name (PostgreSQL keeps 63 bytes).
    /// @p logger  — may be null.
    PgLeaderLock(EventLoop& loop, std::string conninfo, std::string key,
                 std::string holder, std::chrono::seconds interval,
                 Logger* logger);
    ~PgLeaderLock();

    PgLeaderLock(const PgLeaderLock&)            = delete;
    PgLeaderLock& operator=(const PgLeaderLock&) = delete;

    /// Begin: connect and ask at once, then every interval.
    void start(AcquiredHandler on_acquired, LostHandler on_lost);

    bool held() const noexcept { return held_; }

    /// Held, and confirmed by the server within answer_timeout(). After a
    /// stall (a stopped process, a starved host) every ready event is
    /// delivered at once, and a timer may run before the socket that says
    /// the session is gone: held() is still true then, fresh() is not.
    /// Nor after a shorter stall: the lock's socket is asked on the spot, in
    /// any state, whether the server has closed it — and if so that is a loss,
    /// reported from inside this call (the lost handler runs synchronously)
    /// before the caller's work rather than after it.
    /// Timer-driven work of a leader asks this; work arriving on sockets
    /// cannot be ordered that way — a row claim covers that window.
    bool fresh();

    const std::string& key() const noexcept { return key_; }
    std::int64_t lock_id() const noexcept { return lock_id_; }

    /// How long a connect or a query may take before the connection is
    /// given up: max(3 × interval, 15 s).
    std::chrono::seconds answer_timeout() const noexcept;

    /// idle_session_timeout set on the session: 4 × answer_timeout().
    std::chrono::seconds server_timeout() const noexcept;

    /// 64-bit FNV-1a of the key — stable across PostgreSQL versions, unlike
    /// hashtext(). In pg_locks: classid = high 32 bits, objid = low 32 bits,
    /// objsubid = 1.
    static std::int64_t key_hash(std::string_view key) noexcept;

private:
    enum class Ask { none, try_lock, probe };

    void tick();
    void connect();
    void on_io(std::uint32_t events);
    void send(Ask what);
    void handle(std::vector<PgResult>& results);
    void acquired();
    void drop(std::string_view why);
    void close();
    void lose(std::string_view why);
    std::string tag() const;

    EventLoop&           loop_;
    std::string          conninfo_;
    std::string          key_;
    std::string          holder_;
    std::chrono::seconds interval_;
    Logger*              logger_;
    std::int64_t         lock_id_;

    std::unique_ptr<PgConnection> conn_;
    EventLoop::TimerId   timer_{EventLoop::kInvalidTimer};
    Ask                  asked_{Ask::none};
    bool                 set_up_{false};    // this session's SETs were sent
    std::chrono::steady_clock::time_point since_{};  // connect start / query sent
    std::chrono::steady_clock::time_point confirmed_{};  // last "yes, held" from the server

    AcquiredHandler on_acquired_;
    LostHandler     on_lost_;

    bool        held_{false};
    bool        inert_{false};
    std::string last_holder_;   // standby: who held it when we last asked
    bool        waiting_logged_{false};
    std::string last_error_;    // standby: repeated connect errors are logged once
    std::string session_pid_;   // pg_backend_pid() of the first answer on this connection
};

} // namespace apostol

#endif // WITH_POSTGRESQL
