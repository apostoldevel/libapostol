#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"

#include <libpq-fe.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <deque>
#include <queue>   // no longer used here; kept for consumers that got it through this header
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// PgPool::listen() takes a third argument, ListenReadyHandler. Modules are
// pinned against different libapostol revisions across projects; a module
// that wants the hook tests this macro instead of failing to build on an
// older library.
#define APOSTOL_PG_LISTEN_READY 1

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

    /// libpq's error text — or, once withhold_statement() was called, the
    /// reduced one it produced.
    const char*    error_message() const;

    /// For a statement whose text must not be repeated (a quiet query: its
    /// literals are tokens, passwords, secrets). libpq's message quotes the
    /// statement back ("LINE 1: SELECT …"), and DETAIL/CONTEXT and even the
    /// primary message can carry a literal value (invalid input syntax for
    /// type uuid: "<the value>"). From here on error_message() answers
    /// "<SEVERITY>:  <primary message>[; PL/pgSQL function … line N …]
    /// (SQLSTATE <code>)" — the primary message withheld for class 22 and
    /// 42601, where it quotes a value; the context reduced to the PL/pgSQL
    /// location lines without quotes. Enough to tell what failed and where,
    /// nothing of what was sent. No-op on a success.
    ///
    /// Relies on a convention, not a proof: the platform's own messages
    /// (RAISE … 'ERR-…') are kept verbatim, so a message that interpolates a
    /// secret would pass. None does today.
    void           withhold_statement();

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
    std::string withheld_;   // non-empty: error_message() answers this instead
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

    /// Nobody is waiting for this query any more: it still runs, but deliver() and
    /// fail() no longer call its handlers. Set by PgPool::detach() at shutdown.
    bool detached() const      { return detached_; }
    void mark_detached()       { detached_ = true; }

    void deliver(std::vector<PgResult> results);
    void fail   (std::string_view error);

    /// True if on_exception() was given a handler.
    ///
    /// The pool asks before routing a failed statement to fail(): a caller that
    /// installed no error handler still has to be called, or a deferred HTTP
    /// response is simply never sent and the client waits out its timeout. Such a
    /// caller keeps receiving the failed results in on_result — which is what it
    /// already inspected, since that was the only way to see an error at all —
    /// and the pool logs the failure loudly instead of leaving it to it.
    bool has_exception_handler() const noexcept
    { return static_cast<bool>(exception_handler_); }

private:
    static inline uint64_t next_id_{0};

    uint64_t         id_;
    std::string      sql_;
    bool             quiet_{false};
    bool             canceled_{false};
    bool             detached_{false};
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

    /// libpq's connection error text — without the error of a quiet statement
    /// that libpq keeps at its head until the next query is sent (see
    /// withhold_last_error()).
    const char*  error_message() const;

    /// The current connection error text is a quiet statement's error, which
    /// quotes the statement. libpq 14+ keeps it in the connection's buffer
    /// until the next query and appends to it — a connection lost while idle
    /// reported "ERROR: … LINE 1: <the statement>" first. From here until the
    /// next send_query(), error_message() answers only what came after it.
    void         withhold_last_error();

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
    std::string  withheld_error_;   // the head error_message() must not repeat

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

    /// Called when the server has acknowledged LISTEN on `channel`.
    using ListenReadyHandler = std::function<void(std::string_view channel)>;

    /// Subscribe to a PostgreSQL channel.
    /// The callback is invoked from within the EventLoop for each notification.
    /// May be called before or after start().
    ///
    /// `on_ready` fires, from within the EventLoop, each time the server
    /// acknowledges LISTEN on the channel: the first subscription and every
    /// re-subscription after the listener connection is lost and replaced
    /// (the process keeps running; the pool reconnects on its own). A NOTIFY
    /// committed after that moment will be delivered; one committed before it
    /// may have been lost, and the handler is where a consumer looks for it.
    ///
    /// It may fire more often than the connection is lost: subscribing with
    /// `on_ready` to a channel that is already live re-sends LISTEN (a no-op on
    /// the server) so that the new subscriber gets its call, and the answer
    /// fires every `on_ready` of that channel. It never fires on a LISTEN the
    /// server rejected, nor for one shipped on a connection lost before the
    /// answer came — that channel is confirmed by the next listener instead.
    /// A server that rejects every LISTEN (a hot standby: "cannot execute
    /// LISTEN during recovery") therefore never fires it at all.
    void listen(const std::string& channel, NotifyHandler cb,
                ListenReadyHandler on_ready = {});

    /// Unsubscribe from a channel (removes all callbacks for that channel).
    void unlisten(std::string_view channel);

    std::size_t connection_count() const { return conns_.size(); }
    std::size_t queue_size()       const { return queue_.size(); }

    /// Queries queued or already sent whose result somebody still waits for. Zero
    /// means nothing is outstanding — used to end a shutdown drain as soon as it is
    /// done rather than waiting out a timeout. Detached queries are not counted:
    /// nobody waits for them, so there is nothing to drain them for.
    std::size_t outstanding() const;

    /// Stop dispatching notifications and LISTEN confirmations, for good: a handler
    /// registered by listen() — before this call or after it — is not called again.
    /// For shutdown, before the drain of work in flight: a NOTIFY is new work, and
    /// a drain that keeps receiving it never gets to zero.
    void mute_listeners() noexcept;

    /// Stop delivering any query asked for before this call: its handlers are no
    /// longer called. A query already sent runs to its end on the server; a queued
    /// one is sent only if a connection frees up before the pool is destroyed, so
    /// none of them is guaranteed to run. Queries executed after this call are
    /// delivered as usual. Implies mute_listeners().
    ///
    /// For shutdown, between the drain of work in flight and on_stop(): a module
    /// tears down in on_stop() what its callbacks use, and a result that arrived
    /// after that would be handed to the remains (T659 — TaskScheduler released its
    /// BotSession in on_stop(), the drain delivered a late job result into it, and
    /// the process died on a null pointer). The handlers themselves are kept, not
    /// destroyed: what they captured goes with the pool, as it did before.
    void detach() noexcept;

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
    std::deque<std::unique_ptr<PgQuery>>       queue_;
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
    void dispatch_listen_ready();

    std::unique_ptr<PgConnection>                               listener_;
    std::unordered_map<std::string, std::vector<NotifyHandler>> notify_handlers_;
    std::unordered_map<std::string, std::vector<ListenReadyHandler>> ready_handlers_;
    // Set by mute_listeners(): no notification or LISTEN confirmation reaches a handler.
    bool                                                        listeners_muted_{false};
    std::unordered_set<std::string>                             pending_listens_;
    // Shipped by send_pending_listens(), answer not read yet. Only the answer
    // makes a subscription real; the send alone does not.
    std::unordered_set<std::string>                             sent_listens_;
    // Answered, on_ready not called yet. A member, not a local, so that
    // unlisten() and restart_listener() can strike a channel out of it while
    // handlers of the same batch are running.
    std::unordered_set<std::string>                             confirmed_listens_;
    // A batch runs as one implicit transaction: one bad channel rolls back
    // every LISTEN in it. Its channels are then shipped one per query (a
    // subset of pending_listens_), so the one the server refuses is named and
    // the rest subscribe.
    std::unordered_set<std::string>                             single_listens_;
    // Refused by the server on this listener, alone. Stays unsubscribed until
    // the listener is replaced; counted out of the "LISTEN active" line.
    std::unordered_set<std::string>                             rejected_listens_;
    // How many channels the query in flight carried when it was shipped.
    // unlisten() shrinks sent_listens_, so its size cannot tell a lone refusal
    // from a batch rolled back around an innocent channel.
    std::size_t                                                 sent_batch_size_{0};
    // Bumped each time start_listener() creates a connection. Tells a caller
    // that ran handlers whether the listener it held is still the same one —
    // a pointer comparison cannot, since a new object may reuse the address.
    std::uint64_t                                               listener_gen_{0};
};

} // namespace apostol

#endif // WITH_POSTGRESQL
