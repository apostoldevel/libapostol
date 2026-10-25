#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"

#include <libpq-fe.h>

#include <chrono>
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

    /// An error result that carries only @p text — no connection's error
    /// buffer, which for a quiet query may still quote the statement (T306).
    /// What a caller without an error handler receives in on_result when its
    /// statement is lost with the connection (PgPool::fail_inflight_query).
    static PgResult lost(std::string text);

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

    /// The SQLSTATE of an error result ("23505", or one a function raised with
    /// ERRCODE); "" for a success and for a result made without a server (lost()).
    /// The one way to tell one error from another without parsing its text.
    const char* sqlstate() const;

    /// Access the raw handle (for advanced use only; do not call PQclear on it).
    PGresult* handle() const { return res_.get(); }

private:
    std::unique_ptr<PGresult, decltype(&PQclear)> res_;
    std::string withheld_;   // non-empty: error_message() answers this instead
};

// ── PgRetry ───────────────────────────────────────────────────────────────────

/// What the pool does with a query whose connection is lost after the query was
/// sent whole — when the server may or may not have run it (T627).
///
///   never    — it is not sent again: the caller gets an error, "connection lost
///              … may or may not have been executed" (on_exception, or a result
///              that is not ok() when there is none). The default, and the only
///              safe answer for a statement with an effect: repeated, it ran
///              twice — a second row, a second charge — and the caller saw one
///              success.
///   if_lost  — it is sent again on a new connection, as the pool did for every
///              query before T627. For statements whose repetition is harmless
///              or wanted (a read, an idempotent write, a claim that expires),
///              chosen at the call site where that is known.
///
/// A query not yet sent, or not sent whole, is always sent again: the server
/// never had it. A canceled one never is.
enum class PgRetry { never, if_lost };

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

    PgRetry retry() const          { return retry_; }
    void    set_retry(PgRetry r)   { retry_ = r; }

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
    PgRetry          retry_{PgRetry::never};
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
    ///
    /// Blocking: PQcancel opens a new connection to the server and waits for
    /// it to close, on the calling thread. PgPool::cancel() uses it only where
    /// libpq has no non-blocking cancel (before 17).
    bool cancel(std::string& errbuf);

#ifdef LIBPQ_HAS_ASYNC_CANCEL
    /// A cancel request for the query running here, not started yet
    /// (PQcancelCreate): the caller drives it with PQcancelStart/PQcancelPoll
    /// and frees it with PQcancelFinish. nullptr when there is no connection.
    PGcancelConn* cancel_conn() const { return conn_ ? PQcancelCreate(conn_.get()) : nullptr; }
#endif

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

    /// A query is in flight and its answer — or the next part of it — is already
    /// in libpq's buffer: PQgetResult would not block. After a PQconsumeInput
    /// that read it (consume_notify() reads whatever came), no epoll event
    /// announces it any more; the caller has to look.
    bool         answer_buffered() const;
    PgConnState  state()     const { return state_; }
    void         set_state(PgConnState s) { state_ = s; }
    bool         resetting() const { return resetting_; }
    bool         needs_flush() const { return needs_flush_; }

    /// The handshake's time limit, taken at connect_start()/reset_start() from
    /// the options libpq actually uses (PQconninfo: the connection string, the
    /// environment, a service file): connect_timeout, below 2 s 2 s, as in
    /// libpq; zero when connect_timeout is absent or 0 (no limit).
    ///
    /// It exists because the asynchronous PQconnectPoll does not honour
    /// connect_timeout at all: a server that accepts TCP and never answers held
    /// a connection in Connecting for good (T606). The limit is only kept here;
    /// PgPool is what acts on it, through handshake_expired().
    std::chrono::seconds handshake_limit() const { return handshake_limit_; }

    /// Asked once per tick of PgPool's handshake watch. True when the
    /// handshake has been past its limit at this tick AND at the previous one.
    ///
    /// The clock runs only once TCP is up. While PQstatus is
    /// CONNECTION_STARTED the kernel bounds the connect, and a failed connect
    /// is what makes libpq move on to the next address or host — cutting it
    /// short kept every attempt on a first address that drops SYN (an IPv6
    /// black hole) where libpq alone would have reached the second. Each such
    /// move starts the clock again: the limit is per address, as in libpq.
    ///
    /// Two ticks, because after a stalled loop the socket's answer and the
    /// watch's timer come in one epoll batch: a verdict on the first tick
    /// would drop a connection whose answer is being read right then.
    bool handshake_expired(std::chrono::steady_clock::time_point now);

    /// When the server was last heard from with a query in flight: the moment
    /// the query went out, then every read that found input. PgPool's stall
    /// watch (set_stall_timeout()) measures the silence from here.
    std::chrono::steady_clock::time_point heard() const { return heard_; }
    void mark_heard(std::chrono::steady_clock::time_point now) { heard_ = now; }

    /// The backoff round this connection's connect or reset attempt belongs
    /// to (PgPool::record_connect_failure()). Stamped by the pool when the
    /// attempt starts.
    std::uint64_t round() const          { return round_; }
    void          set_round(std::uint64_t r) { round_ = r; }

    /// The query whose cancel request is on its way to this connection's
    /// backend (PgPool::cancel()), 0 — none. While it is, the connection takes
    /// no query: a request arriving after its query finished would cancel the
    /// next one. A reset is a new backend, which the request does not name.
    std::uint64_t cancel_pending() const               { return cancel_pending_; }
    void          set_cancel_pending(std::uint64_t id) { cancel_pending_ = id; }

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

    /// Results at the head of the next batch that belong to the reset sent
    /// ahead of the query (PgPool::set_reset()), not to the query itself.
    std::size_t reset_results() const          { return reset_results_; }
    void        set_reset_results(std::size_t n){ reset_results_ = n; }

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

    /// Start the handshake clock from the options of the live PGconn.
    void start_handshake_clock();

    std::unique_ptr<PGconn, decltype(&PQfinish)> conn_;
    std::chrono::seconds                  handshake_limit_{0};
    std::chrono::steady_clock::time_point handshake_deadline_{};
    bool                                  handshake_late_{false};  // past the limit at the last tick
    std::chrono::steady_clock::time_point heard_{};
    std::uint64_t                         round_{0};
    std::uint64_t                         cancel_pending_{0};
    std::string      conninfo_;
    PgConnState      state_{PgConnState::Connecting};
    PgQuery*         current_query_{nullptr};
    std::size_t      reset_results_{0};
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

    /// Session settings returned to their start-of-session value before every
    /// query on a pool connection, so that a setting one query leaves on the
    /// session (set_config(name, value, false)) is not seen by the next query
    /// that happens to get the same connection. Each entry is a custom
    /// configuration parameter name ("prefix.name"); "all" means RESET ALL —
    /// which also clears what the database code keeps on the session on purpose
    /// (a session cached per connection, for one): use it only where nothing does.
    ///
    /// Sent in the same round trip, ahead of the query ("RESET <name>; <sql>"),
    /// and its results are taken off before the query's own are delivered. The
    /// query therefore runs in one implicit transaction with the reset:
    /// statements that refuse a transaction block — VACUUM, CREATE DATABASE, a
    /// procedure that commits — fail; send those from a pool with an empty list.
    ///
    /// Default {"current.area"}, the per-request scope db-platform keeps in a
    /// session setting. Empty — nothing is sent. Applies to queries sent after
    /// the call. An empty statement goes without it. The LISTEN connection is
    /// never reset.
    /// Throws std::invalid_argument for an entry that is not a parameter name.
    void set_reset(const std::vector<std::string>& names);
    const std::vector<std::string>& reset() const { return reset_names_; }

    /// True if `name` is accepted by set_reset(): "all", or a custom parameter
    /// name — two or more dot-separated parts of letters, digits and '_', each
    /// starting with a letter or '_' and at most 63 bytes long.
    static bool valid_reset_name(std::string_view name);

    /// How long a query in flight may go without a byte from the server before
    /// its connection is given up as dead. Zero (the default) — never.
    ///
    /// The silence counts from the moment the query went out and starts again
    /// with every read that finds input; checked once a second. A connection
    /// past the limit is dropped without a cancel request: the session ends when the
    /// server notices the socket gone (and before libpq 17 a cancel to a dead address
    /// blocks the loop, see cancel()). Its query is failed as one lost with its connection ("may or may
    /// not have been executed"; re-queued if it was not sent whole or is
    /// PgRetry::if_lost), and the connection reconnects in its own slot. The
    /// LISTEN connection is not watched.
    ///
    /// A statement that legitimately runs longer than the limit without
    /// sending anything — a report, a heavy function, pg_sleep — is dropped
    /// too: the limit is a safety net above the longest statement the pool
    /// runs, not a statement timeout. The kernel-level detectors are the
    /// connection string's keepalives_* and tcp_user_timeout.
    void set_stall_timeout(std::chrono::seconds limit) { stall_timeout_ = limit; }
    std::chrono::seconds stall_timeout() const { return stall_timeout_; }

    using QueryId = uint64_t;

    /// Schedule a query. Calls on_result when done, on_error on failure.
    /// Set quiet=true to suppress Query/ResultStatus logging (e.g. heartbeat).
    /// Returns a QueryId that can be passed to cancel().
    /// `retry` — what happens if the connection is lost once the query has gone
    /// out: see PgRetry. The default is not to send it again.
    QueryId execute(std::string              sql,
                    PgQuery::ResultHandler    on_result,
                    PgQuery::ExceptionHandler on_exception = {},
                    bool                      quiet = false,
                    PgRetry                   retry = PgRetry::never);

    /// Cancel a running or queued query.
    /// For in-flight queries: asks PostgreSQL to cancel it; result is silently discarded.
    /// The request goes on a connection of its own, which the loop drives
    /// (PQcancelStart/PQcancelPoll, libpq 17 and later): this returns at once,
    /// and a server that takes the connection and does not answer holds
    /// nothing but that connection, given up after the connection string's
    /// connect_timeout (10 s without one). With an older libpq it is PQcancel,
    /// which blocks the loop until the server answers — against a server that
    /// hangs, for as long as the kernel keeps the connection.
    /// Until the request is done the query's connection takes no other query:
    /// a request arriving after its query finished would cancel the next one.
    /// A request that fails after its packet may have left (the server took the
    /// connection and went silent, or the connection failed) could still arrive: the
    /// connection is reset instead, a new backend the request does not name.
    /// The outcome is logged, not reported: delivered, failed, or given up. A
    /// request still on its way when the pool ends goes with it, and the
    /// query may run to its end on the server.
    /// For queued queries: removed from the queue without dispatching.
    /// For a query lost with its connection and not yet reported: not reported.
    /// A canceled query is never sent again after a connection loss.
    /// Returns true if the query was found and the cancel started (sent, with
    /// an older libpq); a repeated call while a request is on its way starts
    /// no second one.
    bool cancel(QueryId id);

    /// Call periodically (e.g. every 60s from module heartbeat).
    /// Checks connection health, reconnects dead connections, replaces those
    /// left in Error with nothing to drive them, restores min_conns, and arms
    /// the handshake watch if a handshake in flight has none.
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

    /// Run a statement on the LISTEN connection itself rather than on a pool
    /// connection — for what has to ride the subscription: a heartbeat whose
    /// answer proves the listener alive, a registration that records the
    /// listener's backend pid (pg_backend_pid() there is the listener's).
    ///
    /// One at a time, in the order given, when the listener is idle; a LISTEN
    /// waiting to be shipped goes first. Starts the listener if there is none.
    /// Counted by outstanding() — a shutdown drain waits for it — and therefore
    /// bounded by @p answer_within when there is one. Not reset (set_reset()),
    /// not repeated: the
    /// statement in flight when the listener is lost is failed as one lost with
    /// its connection, from the loop; the ones still waiting go out on the next
    /// listener. After detach() its handlers are not called.
    ///
    /// @p answer_within > 0: with no answer by then the listener is given up as
    /// dead and replaced. The time runs from the send, and while the statement
    /// waits behind one without a limit (a LISTEN, the application_name of a
    /// new listener), from the moment it is asked: a server silent on what
    /// precedes it would never get to it. That is the point of sending a heartbeat here — a
    /// listener the network has cut without a word (a black hole, a failover)
    /// hears nothing and libpq does not know; only a question it fails to
    /// answer tells.
    void listener_execute(std::string               sql,
                          PgQuery::ResultHandler    on_result,
                          PgQuery::ExceptionHandler on_exception = {},
                          bool                      quiet = false,
                          std::chrono::seconds      answer_within = {});

    /// application_name of the LISTEN connection, set on every listener once
    /// it connects (by a statement on it — the connection string may be a URI).
    /// Empty — not set. Says in pg_stat_activity whose subscription it is.
    void set_listener_name(std::string name);

    std::size_t connection_count() const { return conns_.size(); }
    std::size_t queue_size()       const { return queue_.size(); }

    /// Queries queued or already sent — on the pool or on the listener
    /// (listener_execute()) — whose result somebody still waits for. Zero
    /// means nothing is outstanding — used to end a shutdown drain as soon as it is
    /// done rather than waiting out a timeout. Detached queries are not counted:
    /// nobody waits for them, so there is nothing to drain them for.
    std::size_t outstanding() const;

    /// Fail the queries that have nowhere to run — queued on the pool while
    /// none of its connections is up or connecting, or on the listener while
    /// it is down — unless a reconnect is due within @p patience. They would
    /// wait for that reconnect, with the database gone for as long as the
    /// outage lasts. The backoff says when the next attempt is, not whether
    /// the server answers: the first call that finds a side (pool, listener)
    /// stranded starts an attempt at once instead and fails nothing of it; a
    /// later one fails its queries once that attempt has failed — a server
    /// back between two attempts is found. Cancelled queries still queued are
    /// dropped by every call (their results are discarded anyway). The caller
    /// hears @p reason — on_exception(), or a result that is not ok() — as for
    /// a query lost with its connection, and before
    /// this returns. For shutdown, in the finish of the work in flight: a stop
    /// during an outage would otherwise wait out "process.stop_timeout", and
    /// an HTTP request behind such a query would get no answer at all.
    /// Returns how many were failed.
    std::size_t fail_stranded(std::string_view reason, std::chrono::milliseconds patience);

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
    /// Queries executed after this call go out ahead of the ones it detached: what
    /// on_stop() sends (a deregistration, a sign-out) is not left behind work that
    /// nobody waits for, when every connection is busy.
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

    /// Send `q` on `conn` behind the reset (set_reset()) and note on the
    /// connection how many results at the head of the batch are the reset's.
    bool send_query(PgConnection& conn, const PgQuery& q);

    /// Take the reset's results off the head of a complete batch. A reset that
    /// failed stays: the query behind it did not run, and its caller learns why.
    static void strip_reset_results(PgConnection& conn, std::vector<PgResult>& results);

    /// Attempt to reconnect a connection via PQresetStart.
    /// Handles fd change (remove old epoll, add new).
    /// Returns true if reset was initiated successfully.
    bool try_reconnect(PgConnection& conn);

    /// Remove a dead connection from the pool and create a replacement.
    void replace_connection(PgConnection& conn);

    /// Register a pool connection's socket with the loop. When epoll refuses
    /// (T789) the connection is put in Error before the exception goes on:
    /// left as it was — Connecting, or Ready — nothing would drive it, yet
    /// healthy_count() would count it. heartbeat() replaces it.
    void watch(PgConnection& conn, uint32_t events);

    /// watch() for on_io()'s handshake, where nothing above holds the
    /// connection: refused, it is replaced at once. False — it is gone.
    bool watch_or_replace(PgConnection& conn, uint32_t events);

    /// Take the in-flight query off a lost connection: re-queue it if it was not
    /// sent whole or is PgRetry::if_lost, drop it if canceled, otherwise hand it
    /// to lost_ to be failed from the loop.
    void fail_inflight_query(PgConnection& conn, std::string_view reason);

    /// Tell the callers in lost_ — from the loop, on a clean stack.
    void deliver_lost();

    /// Ensure at least min_conns_ healthy connections exist.
    void ensure_min_connections();

    /// Count connections in Ready, Busy or Connecting state with a socket.
    std::size_t healthy_count() const;

    /// Busy, a stall limit set, and nothing heard for longer than it.
    bool stalled(const PgConnection& conn, std::chrono::steady_clock::time_point now) const;

    /// Format v1-style connection tag: [backend_pid] [fd] [postgresql://user@host:port/dbname]
    std::string conn_tag(const PgConnection& conn) const;

    /// Install notice callback on a connection (forwards to pg_logger_)
    void setup_notice_handler(PgConnection& conn);

    EventLoop&  loop_;
    std::string conninfo_;
    std::size_t min_conns_;
    std::size_t max_conns_;
    Logger*     pg_logger_{nullptr};

    std::vector<std::string> reset_names_;
    std::string              reset_sql_;     // "RESET a; RESET b; " — prefix of every query
    std::size_t              reset_count_{0}; // statements in reset_sql_

    std::vector<std::unique_ptr<PgConnection>> conns_;
    std::deque<std::unique_ptr<PgQuery>>       queue_;
    std::vector<std::unique_ptr<PgQuery>>      inflight_;     // queries currently executing
    std::unordered_set<uint64_t>               canceled_ids_; // queued queries pending cancel

    // Exponential backoff for connect failures: prevents auth-fail / unreachable-host
    // tight loop from hammering PG with retries at full event-loop speed (and
    // flooding logs).  Counter caps at 6 → max delay 32s.  Cleared on first
    // successful connect. Counted once per round of attempts, not once per
    // connection (record_connect_failure()).
    std::chrono::steady_clock::time_point next_connect_attempt_{};
    int                                    consecutive_connect_fails_{0};
    std::uint64_t                          connect_round_{0};
    EventLoop::TimerId                     reconnect_timer_{EventLoop::kInvalidTimer};
    // fail_stranded() started an attempt past the backoff for the pool's
    // queue (the listener's), and no connect of the pool (of the listener)
    // has succeeded since: the next stranded call fails that side's queries.
    bool                                   pool_probed_{false};
    bool                                   listener_probed_{false};

    // Sent whole, lost with the connection, not to be repeated: waiting for
    // deliver_lost() to tell their callers. Kept here rather than in a bare timer
    // so that outstanding() counts them and detach() reaches them — a failure
    // delivered to a module after its on_stop() is exactly T659.
    struct Lost
    {
        std::unique_ptr<PgQuery> query;
        std::string              error;
        PgResult                 result;   // for a caller without an error handler
    };
    std::vector<Lost>                      lost_;
    EventLoop::TimerId                     lost_timer_{EventLoop::kInvalidTimer};

#ifdef LIBPQ_HAS_ASYNC_CANCEL
    // A cancel request in flight on its own connection (cancel()): the loop
    // drives it, a timer gives it up. Owned here, so that the pool's end ends
    // it as well.
    struct CancelRequest
    {
        std::unique_ptr<PGcancelConn, decltype(&PQcancelFinish)> conn{nullptr, &PQcancelFinish};
        uint64_t           query_id{0};
        std::string        tag;     // conn_tag() of the connection the query runs on
        PgConnection*      target{nullptr};  // that connection; nullptr once it is replaced
        int                fd{-1};  // registered with the loop, -1 — not
        bool               sent{false};  // its packet may have left: connected, or awaiting the server
        EventLoop::TimerId timer{EventLoop::kInvalidTimer};
        std::chrono::seconds limit{0};
    };
    std::vector<std::unique_ptr<CancelRequest>> cancels_;

    /// Start a cancel request for the query on @p conn. False — not started.
    bool start_cancel(PgConnection& conn, uint64_t id, std::string& error);
    /// The cancel connection's socket is ready for what PQcancelPoll asked.
    void on_cancel_io(CancelRequest* cr);
    /// Done, failed or given up: taken off the loop and destroyed, logged; its
    /// connection free to take queries again, or reset when the request may
    /// still arrive.
    /// @p timed_out — given up by its timer.
    void end_cancel(CancelRequest* cr, bool ok, std::string_view error, bool timed_out = false);
    /// Taken off the loop and destroyed, nothing else.
    void drop_cancel(CancelRequest* cr) noexcept;
#endif

    void record_connect_success();

    /// The attempt the reconnect timer would make, made now: the backoff
    /// window is dropped (fail_stranded()). Throws what the loop throws.
    void reconnect_now();

    /// A connect or reset attempt of round `round` failed. Only the first
    /// failure of the current round moves the backoff a step and opens the
    /// next round; the attempts started alongside it — the rest of the pool,
    /// the listener — fail into a round already counted and change nothing
    /// (a pool of five and its listener against one dead server used to
    /// count six steps for one outage).
    void record_connect_failure(std::uint64_t round);

    /// Arm reconnect_timer_ (if not already armed) to fire once the current
    /// backoff window elapses. The callback restores min_conns_ AND, if a
    /// LISTEN/NOTIFY subscription exists but listener_ is down, restarts it —
    /// shared so whichever side (pool or listener) hits the failure first
    /// still recovers the other. Used by both new_connection() and the
    /// listener connect-failure paths, so a dedicated LISTEN connection
    /// failing during startup isn't abandoned forever (was: listener_.reset()
    /// with no retry — see start_listener()/on_listener_io()).
    void schedule_reconnect_timer();

    // ── Handshake watch (T606) ────────────────────────────────────────────────
    // PQconnectPoll keeps no time: a server that takes the TCP connection and
    // never answers leaves the handshake waiting for an event that will not
    // come, and heartbeat()/listener_is_dead() leave Connecting alone. One
    // one-shot timer, armed whenever a handshake with a limit starts and
    // re-armed while any is in flight, asks each one handshake_expired() once
    // a second; an expired one is dropped as a failed connect — pool or
    // listener alike, with the backoff. A timer the loop refuses (T789) is
    // logged, not thrown: the handshake goes on without its limit until the
    // next start or heartbeat() arms the watch again.
    void arm_handshake_watch() noexcept;
    void on_handshake_watch();
    bool handshake_in_flight() const;   // a pool connection or the listener, with a limit
    EventLoop::TimerId handshake_timer_{EventLoop::kInvalidTimer};

    // ── Stall watch (set_stall_timeout()) ─────────────────────────────────────
    // One one-shot timer, armed when a query goes out and re-armed once a
    // second while any connection is Busy. Refused by the loop, it is logged,
    // not thrown; heartbeat() runs the same check.
    void arm_stall_watch() noexcept;
    void on_stall_watch();
    void drop_stalled(std::chrono::steady_clock::time_point now);
    std::chrono::seconds stall_timeout_{0};
    EventLoop::TimerId   stall_timer_{EventLoop::kInvalidTimer};

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
    /// while a handshake is still in flight (its limit is the handshake
    /// watch's to judge), and false when there is no listener at all (nothing
    /// to restart; see the callers).
    bool listener_is_dead() const;

    /// A listener exists, has a socket and is not dead — connected, or with
    /// its handshake in flight.
    bool listener_reachable() const;

    void on_listener_io(uint32_t events);
    void send_pending_listens();
    void dispatch_notify(const char* channel, const char* payload);
    void dispatch_listen_ready();

    /// A channel to listen on, or a listener_execute() statement waiting: the
    /// listener has to exist.
    bool wants_listener() const { return !notify_handlers_.empty() || !listener_queue_.empty(); }

    /// The listener is Ready: ship what waits for it — a LISTEN first, else the
    /// next listener_execute() statement. Does nothing on a busy listener.
    void listener_send_next();

    /// The answer to the statement in flight on the listener: hand it over.
    void deliver_listener_query(std::vector<PgResult> results);

    /// The listener is lost with a statement in flight: fail it from the loop.
    void fail_listener_query(std::string_view reason);

    void arm_listener_answer(std::chrono::seconds limit);
    void cancel_listener_answer();

    /// The listener is busy with a statement nobody set a limit for — a LISTEN
    /// batch, the SET application_name of a new listener — and a statement
    /// with a limit waits behind it: the shortest such limit runs from now.
    void watch_listener_queue();

    /// Queue a query: after detach(), one somebody waits for goes ahead of the
    /// detached ones.
    void enqueue(std::unique_ptr<PgQuery> q);

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

    // listener_execute(): waiting, and the one in flight (at most one; the
    // listener answers one statement at a time).
    struct ListenerQuery
    {
        std::unique_ptr<PgQuery> query;
        std::chrono::seconds     answer_within{0};
    };
    std::deque<ListenerQuery>                                   listener_queue_;
    std::unique_ptr<PgQuery>                                    listener_query_;
    EventLoop::TimerId                                          listener_answer_timer_{EventLoop::kInvalidTimer};
    // Notification handlers running: consume_notify() is iterating the
    // listener's PGconn, and nothing may replace the listener under it.
    int                                                         notify_depth_{0};
    // Bumped by every answer the listener reads: tells watch_listener_queue()
    // whether the server said anything while it waited.
    std::uint64_t                                               listener_answers_{0};
    std::string                                                 listener_name_;
    bool                                                        detached_{false};
};

} // namespace apostol

#endif // WITH_POSTGRESQL
