#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"

#include <libpq-fe.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace apostol
{

// ── PgResult ──────────────────────────────────────────────────────────────────

/// RAII wrapper over PGresult*.
/// Owns the result object; PQclear() is called in the destructor.
class PgResult
{
public:
    explicit PgResult(PGresult* res);
    ~PgResult();

    PgResult(const PgResult&)            = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&&) noexcept;
    PgResult& operator=(PgResult&&) noexcept;

    ExecStatusType status() const;
    const char*    status_string() const;
    const char*    error_message() const;

    int rows()    const;   // PQntuples
    int columns() const;   // PQnfields

    const char* column_name(int col) const;          // PQfname
    int         column_index(const char* name) const; // PQfnumber
    Oid         column_type(int col) const;           // PQftype

    bool        is_null(int row, int col) const;  // PQgetisnull
    const char* value   (int row, int col) const; // PQgetvalue
    int         length  (int row, int col) const; // PQgetlength

    /// true if status == PGRES_TUPLES_OK or PGRES_COMMAND_OK.
    bool ok() const;

    /// Access the raw handle (for advanced use only; do not call PQclear on it).
    PGresult* handle() const { return res_.get(); }

private:
    std::unique_ptr<PGresult, decltype(&PQclear)> res_;
};

// ── PgQuery ───────────────────────────────────────────────────────────────────

/// An asynchronous query with completion callbacks.
class PgQuery
{
public:
    using ResultHandler    = std::function<void(std::vector<PgResult>)>;
    using ExceptionHandler = std::function<void(std::string_view)>;

    explicit PgQuery(std::string sql, bool quiet = false);

    PgQuery& on_result   (ResultHandler    h);
    PgQuery& on_exception(ExceptionHandler h);

    uint64_t           id()    const { return id_; }
    const std::string& sql()   const { return sql_; }
    bool               quiet() const { return quiet_; }

    bool canceled() const      { return canceled_; }
    void mark_canceled()       { canceled_ = true; }

    void deliver(std::vector<PgResult> results);
    void fail   (std::string_view error);

private:
    static inline uint64_t next_id_{0};

    uint64_t         id_;
    std::string      sql_;
    bool             quiet_{false};
    bool             canceled_{false};
    ResultHandler    result_handler_;
    ExceptionHandler exception_handler_;
};

// ── PgConnection ──────────────────────────────────────────────────────────────

enum class PgConnState { Connecting, Ready, Busy, Error };

/// A single async libpq connection.
/// Integrates with EventLoop: register fd with add_io; call on_readable/on_writable
/// from the I/O callback.
class PgConnection
{
public:
    explicit PgConnection(std::string conninfo);
    ~PgConnection();

    PgConnection(const PgConnection&)            = delete;
    PgConnection& operator=(const PgConnection&) = delete;
    PgConnection(PgConnection&&)                 = delete;
    PgConnection& operator=(PgConnection&&)      = delete;

    /// Start non-blocking connect. Returns false on immediate failure.
    bool connect_start();

    /// Poll the connection handshake (call when fd is readable or writable).
    /// Returns PGRES_POLLING_OK when the connection is established.
    PostgresPollingStatusType connect_poll();

    /// Start an async connection reset.
    bool reset_start();

    /// Poll a reset (same epoll event routing as connect_poll).
    PostgresPollingStatusType reset_poll();

    /// Send a query asynchronously. Connection must be in Ready state.
    /// Returns false on error.
    bool send_query(const std::string& sql);

    /// Cancel the currently running query via PQcancel (out-of-band signal).
    /// On failure, writes the error into errbuf and returns false.
    bool cancel(std::string& errbuf);

    /// Consume input and collect all completed results.
    /// Call when fd becomes readable in Busy state.
    std::vector<PgResult> collect_results();

    /// Flush the output buffer. Returns true when fully flushed.
    bool flush();

    /// Check for NOTIFY messages. Calls cb for each one; returns the count,
    /// or -1 when PQconsumeInput failed. On -1 libpq has already closed the
    /// socket and the connection has been moved to Error — the caller MUST
    /// react here, because no epoll event can follow a closed socket.
    /// fd() reports -1 from that moment on — it asks libpq every time — while
    /// last_fd() still names the number the socket had.
    int consume_notify(const std::function<void(const char* channel, const char* payload)>& cb);

    /// The connection's socket, asked of libpq every time — NOT a cached copy.
    /// It was a cache until 2026-08-26, refreshed only by connect/reset polling,
    /// and that is a defect generator rather than an optimisation: libpq closes
    /// the socket by itself on any failed read, and every one of the dozen
    /// places that set Error would have had to remember to refresh it. Miss one
    /// and a stale number reaches loop_.remove_io(), which then deregisters
    /// whichever connection the kernel has since handed that number — the pools
    /// share one EventLoop, so the victim is some other connection going quietly
    /// deaf, nowhere near the mistake. PQsocket is a struct field read, not a
    /// syscall, and returns -1 for a null handle on its own.
    int          fd()        const { return conn_ ? PQsocket(conn_.get()) : -1; }

    /// The last socket number this connection was SEEN to have. fd() reports -1
    /// once the socket is gone, which is honest and useless to a reader: "-1"
    /// matches nothing in a log. Print the pair — "fd=22 gone" — so the next
    /// defect of this shape stays catchable by its number, the way this one was.
    int          last_fd()   const { return last_fd_; }
    bool         connected() const;
    PgConnState  state()     const { return state_; }
    void         set_state(PgConnState s) { state_ = s; }
    bool         resetting() const { return resetting_; }
    bool         needs_flush() const { return needs_flush_; }

    const char*  error_message() const;

    PgQuery* current_query() const        { return current_query_; }
    void     set_current_query(PgQuery* q){ current_query_ = q; }

    // ── libpq conninfo accessors (valid after connect) ───────────────────────
    const char* pg_host()     const { return conn_ ? PQhost(conn_.get())       : ""; }
    const char* pg_port()     const { return conn_ ? PQport(conn_.get())       : ""; }
    const char* pg_user()     const { return conn_ ? PQuser(conn_.get())       : ""; }
    const char* pg_dbname()   const { return conn_ ? PQdb(conn_.get())         : ""; }
    int         backend_pid() const { return conn_ ? PQbackendPID(conn_.get()) : 0; }

    // ── Notice callback (set by PgPool for logging) ──────────────────────────
    using NoticeCallback = std::function<void(const char* message)>;
    void set_notice_callback(NoticeCallback cb);

private:
    static void notice_processor(void* arg, const char* message);

    std::unique_ptr<PGconn, decltype(&PQfinish)> conn_;
    std::string      conninfo_;
    PgConnState      state_{PgConnState::Connecting};
    PgQuery*         current_query_{nullptr};
    int              last_fd_{-1};
    bool             resetting_{false};
    bool             needs_flush_{false};
    NoticeCallback   notice_cb_;

    /// Accumulator for multi-statement query results across split-TCP deliveries.
    /// Results are collected here until PQgetResult returns NULL (all statements
    /// complete), then returned as a single batch to prevent partial delivery.
    std::vector<PgResult> pending_results_;
};

// ── PgPool ────────────────────────────────────────────────────────────────────

/// Connection pool.
/// Owns N PgConnections, integrates with EventLoop.
/// execute() dispatches to a ready connection or queues the query.
///
/// Reconnect strategy (mirrors v1 CPQConnectPoll):
/// - On Error state: attempt PQresetStart() to reconnect in-place
/// - On dispatch: verify PQstatus() before sending queries
/// - Periodic heartbeat: check health, restore min_conns, cleanup idle
class PgPool
{
public:
    PgPool(EventLoop&   loop,
           std::string  conninfo,
           std::size_t  min_conns = 1,
           std::size_t  max_conns = 5,
           Logger*      pg_logger = nullptr);
    ~PgPool();

    PgPool(const PgPool&)            = delete;
    PgPool& operator=(const PgPool&) = delete;

    /// Open min_conns connections and register them with the event loop.
    void start();

    using QueryId = uint64_t;

    /// Schedule a query. Calls on_result when done, on_error on failure.
    /// Set quiet=true to suppress Query/ResultStatus logging (e.g. heartbeat).
    /// Returns a QueryId that can be passed to cancel().
    QueryId execute(std::string              sql,
                    PgQuery::ResultHandler    on_result,
                    PgQuery::ExceptionHandler on_exception = {},
                    bool                      quiet = false);

    /// Cancel a running or queued query.
    /// For in-flight queries: sends PQcancel to PostgreSQL; result is silently discarded.
    /// For queued queries: removed from the queue without dispatching.
    /// Returns true if the query was found and cancel was initiated.
    bool cancel(QueryId id);

    /// Call periodically (e.g. every 60s from module heartbeat).
    /// Checks connection health, reconnects dead connections, restores min_conns.
    void heartbeat();

    // ── LISTEN / NOTIFY ───────────────────────────────────────────────────────

    using NotifyHandler = std::function<void(std::string_view channel,
                                             std::string_view payload)>;

    /// Subscribe to a PostgreSQL channel.
    /// The callback is invoked from within the EventLoop for each notification.
    /// May be called before or after start().
    void listen(const std::string& channel, NotifyHandler cb);

    /// Unsubscribe from a channel (removes all callbacks for that channel).
    void unlisten(std::string_view channel);

    std::size_t connection_count() const { return conns_.size(); }
    std::size_t queue_size()       const { return queue_.size(); }

    /// Queries queued or already sent and awaiting a result. Zero means nothing is
    /// outstanding — used to end a shutdown drain as soon as it is done rather than
    /// waiting out a timeout.
    std::size_t outstanding() const;

private:
    void new_connection();
    void on_io(PgConnection& conn, uint32_t events);
    void dispatch_queue(PgConnection& conn);

    /// Attempt to reconnect a connection via PQresetStart.
    /// Handles fd change (remove old epoll, add new).
    /// Returns true if reset was initiated successfully.
    bool try_reconnect(PgConnection& conn);

    /// Remove a dead connection from the pool and create a replacement.
    void replace_connection(PgConnection& conn);

    /// Fail any in-flight query on this connection and re-queue it if retriable.
    void fail_inflight_query(PgConnection& conn, std::string_view reason);

    /// Ensure at least min_conns_ healthy connections exist.
    void ensure_min_connections();

    /// Count connections in Ready or Busy state.
    std::size_t healthy_count() const;

    /// Format v1-style connection tag: [backend_pid] [fd] [postgresql://user@host:port/dbname]
    std::string conn_tag(const PgConnection& conn) const;

    /// Install notice callback on a connection (forwards to pg_logger_)
    void setup_notice_handler(PgConnection& conn);

    EventLoop&  loop_;
    std::string conninfo_;
    std::size_t min_conns_;
    std::size_t max_conns_;
    Logger*     pg_logger_{nullptr};

    std::vector<std::unique_ptr<PgConnection>> conns_;
    std::queue<std::unique_ptr<PgQuery>>       queue_;
    std::vector<std::unique_ptr<PgQuery>>      inflight_;     // queries currently executing
    std::unordered_set<uint64_t>               canceled_ids_; // queued queries pending cancel

    // Exponential backoff for connect failures: prevents auth-fail / unreachable-host
    // tight loop from hammering PG with retries at full event-loop speed (and
    // flooding logs).  Counter caps at 6 → max delay 60s.  Cleared on first
    // successful connect.
    std::chrono::steady_clock::time_point next_connect_attempt_{};
    int                                    consecutive_connect_fails_{0};
    EventLoop::TimerId                     reconnect_timer_{EventLoop::kInvalidTimer};

    void record_connect_success();
    void record_connect_failure();

    /// Arm reconnect_timer_ (if not already armed) to fire once the current
    /// backoff window elapses. The callback restores min_conns_ AND, if a
    /// LISTEN/NOTIFY subscription exists but listener_ is down, restarts it —
    /// shared so whichever side (pool or listener) hits the failure first
    /// still recovers the other. Used by both new_connection() and the
    /// listener connect-failure paths, so a dedicated LISTEN connection
    /// failing during startup isn't abandoned forever (was: listener_.reset()
    /// with no retry — see start_listener()/on_listener_io()).
    void schedule_reconnect_timer();

    // Pointer set of connections currently registered with EventLoop.
    // Used by on_io to decide whether it's safe to rearm a conn after a
    // handler may have destroyed it via replace_connection (which erases
    // from conns_, invalidating the reference passed to on_io).
    std::unordered_set<PgConnection*>          registered_conns_;

    // ── Listener (dedicated connection for LISTEN/NOTIFY) ─────────────────────
    void start_listener();

    /// Drop a dead listener and start a fresh one, re-arming every channel
    /// from notify_handlers_. Re-arming reads the REGISTRY, never a literal
    /// list of channels: WebSocketAPI subscribes to channels read from the
    /// database at runtime, so anything enumerating channels statically would
    /// silently drop all of them and take the whole dashboard publication
    /// with it. `reason` is logged.
    void restart_listener(std::string_view reason);

    /// True when the listener object has outlived its connection — libpq
    /// dropped the socket, or PQstatus says the connection is gone. False
    /// while a handshake is still in flight, and false when there is no
    /// listener at all (nothing to restart; see the callers).
    bool listener_is_dead() const;

    void on_listener_io(uint32_t events);
    void send_pending_listens();
    void dispatch_notify(const char* channel, const char* payload);

    std::unique_ptr<PgConnection>                               listener_;
    std::unordered_map<std::string, std::vector<NotifyHandler>> notify_handlers_;
    std::unordered_set<std::string>                             pending_listens_;
};

} // namespace apostol

#endif // WITH_POSTGRESQL
