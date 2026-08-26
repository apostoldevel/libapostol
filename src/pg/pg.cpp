#include "apostol/pg.hpp"

#include <fmt/format.h>
#include <sys/epoll.h>

#include <algorithm>

namespace apostol
{

// ── PgResult ──────────────────────────────────────────────────────────────────

PgResult::PgResult(PGresult* res)
    : res_(res, &PQclear)
{
}

PgResult::~PgResult() = default;

PgResult::PgResult(PgResult&& o) noexcept
    : res_(std::move(o.res_))
{
}

PgResult& PgResult::operator=(PgResult&& o) noexcept
{
    if (this != &o)
        res_ = std::move(o.res_);
    return *this;
}

ExecStatusType PgResult::status() const
{
    return PQresultStatus(res_.get());
}

const char* PgResult::status_string() const
{
    return PQresStatus(status());
}

const char* PgResult::error_message() const
{
    return PQresultErrorMessage(res_.get());
}

int PgResult::rows() const    { return PQntuples(res_.get()); }
int PgResult::columns() const { return PQnfields(res_.get()); }

const char* PgResult::column_name(int col) const    { return PQfname(res_.get(), col); }
int         PgResult::column_index(const char* n) const { return PQfnumber(res_.get(), n); }
Oid         PgResult::column_type(int col) const    { return PQftype(res_.get(), col); }

bool        PgResult::is_null(int row, int col) const { return PQgetisnull(res_.get(), row, col) != 0; }
const char* PgResult::value  (int row, int col) const { return PQgetvalue(res_.get(), row, col); }
int         PgResult::length (int row, int col) const { return PQgetlength(res_.get(), row, col); }

bool PgResult::ok() const
{
    auto s = status();
    return s == PGRES_TUPLES_OK || s == PGRES_COMMAND_OK;
}

// ── PgQuery ───────────────────────────────────────────────────────────────────

PgQuery::PgQuery(std::string sql, bool quiet)
    : id_(++next_id_)
    , sql_(std::move(sql))
    , quiet_(quiet)
{
}

PgQuery& PgQuery::on_result(ResultHandler h)
{
    result_handler_ = std::move(h);
    return *this;
}

PgQuery& PgQuery::on_exception(ExceptionHandler h)
{
    exception_handler_ = std::move(h);
    return *this;
}

void PgQuery::deliver(std::vector<PgResult> results)
{
    if (result_handler_)
        result_handler_(std::move(results));
}

void PgQuery::fail(std::string_view error)
{
    if (exception_handler_)
        exception_handler_(error);
}

// ── PgConnection ──────────────────────────────────────────────────────────────

PgConnection::PgConnection(std::string conninfo)
    : conn_(nullptr, &PQfinish)
    , conninfo_(std::move(conninfo))
{
}

PgConnection::~PgConnection() = default;

void PgConnection::set_notice_callback(NoticeCallback cb)
{
    notice_cb_ = std::move(cb);
    if (conn_)
        PQsetNoticeProcessor(conn_.get(), &PgConnection::notice_processor, this);
}

void PgConnection::notice_processor(void* arg, const char* message)
{
    auto* self = static_cast<PgConnection*>(arg);
    if (self->notice_cb_)
        self->notice_cb_(message);
}

bool PgConnection::connect_start()
{
    PGconn* raw = PQconnectStart(conninfo_.c_str());
    conn_.reset(raw);

    if (!raw || PQstatus(raw) == CONNECTION_BAD) {
        state_ = PgConnState::Error;
        return false;
    }

    if (PQsetnonblocking(raw, 1) == -1) {
        state_ = PgConnState::Error;
        return false;
    }

    // Install notice processor if callback is set
    if (notice_cb_)
        PQsetNoticeProcessor(raw, &PgConnection::notice_processor, this);

    last_fd_ = PQsocket(raw);  // remembered for the log; fd() asks libpq itself
    resetting_ = false;
    return true;
}

PostgresPollingStatusType PgConnection::connect_poll()
{
    auto ps = PQconnectPoll(conn_.get());
    last_fd_ = PQsocket(conn_.get());  // docs: re-determine socket after each poll

    if (ps == PGRES_POLLING_OK)
        state_ = PgConnState::Ready;
    else if (ps == PGRES_POLLING_FAILED)
        state_ = PgConnState::Error;

    return ps;
}

bool PgConnection::reset_start()
{
    pending_results_.clear();
    if (!PQresetStart(conn_.get())) {
        state_ = PgConnState::Error;
        return false;
    }
    PQsetnonblocking(conn_.get(), 1);  // restore nonblocking after reset
    state_ = PgConnState::Connecting;
    resetting_ = true;
    last_fd_ = PQsocket(conn_.get());
    return true;
}

PostgresPollingStatusType PgConnection::reset_poll()
{
    auto ps = PQresetPoll(conn_.get());
    last_fd_ = PQsocket(conn_.get());  // docs: re-determine socket after each poll

    if (ps == PGRES_POLLING_OK)
        state_ = PgConnState::Ready;
    else if (ps == PGRES_POLLING_FAILED)
        state_ = PgConnState::Error;

    return ps;
}

bool PgConnection::connected() const
{
    return conn_ && PQstatus(conn_.get()) == CONNECTION_OK;
}

bool PgConnection::cancel(std::string& errbuf)
{
    if (!conn_) {
        errbuf = "no connection";
        return false;
    }

    PGcancel* pgcancel = PQgetCancel(conn_.get());
    if (!pgcancel) {
        errbuf = "PQgetCancel returned null";
        return false;
    }

    char buf[256]{};
    int ok = PQcancel(pgcancel, buf, sizeof(buf));
    PQfreeCancel(pgcancel);

    if (!ok) {
        errbuf = buf;
        return false;
    }
    return true;
}

bool PgConnection::send_query(const std::string& sql)
{
    if (!connected())
        return false;
    if (PQsendQuery(conn_.get(), sql.c_str()) == 0)
        return false;
    int flush_ret = PQflush(conn_.get());
    if (flush_ret == -1)
        return false;  // flush error — treat as send failure
    needs_flush_ = (flush_ret == 1);  // 1 = more data to send, need EPOLLOUT
    state_ = PgConnState::Busy;
    return true;
}

std::vector<PgResult> PgConnection::collect_results()
{
    if (PQconsumeInput(conn_.get()) == 0) {
        state_ = PgConnState::Error;  // connection is dead; fd() now reports -1
        pending_results_.clear();
        return {};
    }

    // PQisBusy returns 1 if the query is still in progress
    while (PQisBusy(conn_.get()) == 0) {
        PGresult* r = PQgetResult(conn_.get());
        if (!r) {
            // NULL → all results for this query are done, connection is idle.
            // Only transition to Ready here — PQsendQuery requires PQgetResult
            // to have returned NULL before a new query can be sent.
            state_ = PgConnState::Ready;
            break;
        }
        pending_results_.emplace_back(r);
    }

    // Return results only when ALL statements in the multi-statement query
    // are complete (PQgetResult returned NULL → state is Ready).  This prevents
    // split-TCP delivery from causing partial result sets: if the response spans
    // multiple TCP segments, earlier EPOLLIN events may deliver only some results
    // while PQisBusy==1 for the rest.  Accumulating in pending_results_ ensures
    // the callback always receives the full result vector.
    if (state_ == PgConnState::Ready) {
        auto out = std::move(pending_results_);
        pending_results_.clear();
        return out;
    }

    return {};  // still busy — wait for remaining results
}

bool PgConnection::flush()
{
    int ret = PQflush(conn_.get());
    if (ret == 0)
        needs_flush_ = false;
    return ret == 0;
}

int PgConnection::consume_notify(
    const std::function<void(const char* channel, const char* payload)>& cb)
{
    // PQconsumeInput returns 0 on failure, and by then libpq has already
    // closed the socket (PQsocket -> -1) while keeping the PGconn object
    // alive. Discarding that result is how an established LISTEN died for
    // good: the connection stayed Ready, PgPool::listen() only creates a
    // listener when listener_ is empty, and no epoll event can ever arrive
    // on a closed fd. collect_results() above has always checked this — the
    // check was simply missing here, which is why the failure hit the
    // listener (idle in Ready) and not the worker connections (Busy).
    bool alive = (PQconsumeInput(conn_.get()) != 0);

    int count = 0;
    PGnotify* notify;
    // Drain what libpq already parsed even when the read failed: those
    // notifications did arrive, and dropping them would turn one dead socket
    // into lost work.
    while ((notify = PQnotifies(conn_.get())) != nullptr) {
        if (cb)
            cb(notify->relname, notify->extra);
        PQfreemem(notify);
        ++count;
        if (alive)
            alive = (PQconsumeInput(conn_.get()) != 0);
    }

    if (!alive) {
        // libpq has closed the socket by now; fd() reports -1 from here on,
        // so every caller downstream sees the truth without being told.
        state_ = PgConnState::Error;
        return -1;
    }

    return count;
}

const char* PgConnection::error_message() const
{
    return conn_ ? PQerrorMessage(conn_.get()) : "";
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static const char* polling_status_name(PostgresPollingStatusType ps) noexcept
{
    switch (ps) {
        case PGRES_POLLING_FAILED:  return "PGRES_POLLING_FAILED";
        case PGRES_POLLING_READING: return "PGRES_POLLING_READING";
        case PGRES_POLLING_WRITING: return "PGRES_POLLING_WRITING";
        case PGRES_POLLING_OK:      return "PGRES_POLLING_OK";
        default:                    return "PGRES_POLLING_ACTIVE";
    }
}

// ── PgPool ────────────────────────────────────────────────────────────────────

PgPool::PgPool(EventLoop& loop, std::string conninfo,
               std::size_t min_conns, std::size_t max_conns,
               Logger* pg_logger)
    : loop_(loop)
    , conninfo_(std::move(conninfo))
    , min_conns_(min_conns)
    , max_conns_(max_conns)
    , pg_logger_(pg_logger)
{
}

std::string PgPool::conn_tag(const PgConnection& conn) const
{
    return fmt::format("[{}] [{}] [postgresql://{}@{}:{}/{}]",
        conn.backend_pid(), conn.fd(),
        conn.pg_user(), conn.pg_host(), conn.pg_port(), conn.pg_dbname());
}

void PgPool::setup_notice_handler(PgConnection& conn)
{
    if (!pg_logger_)
        return;
    conn.set_notice_callback([this, &conn](const char* message) {
        pg_logger_->notice("{} Processor message: {}", conn_tag(conn), message);
    });
}

PgPool::~PgPool()
{
    if (reconnect_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(reconnect_timer_);
        reconnect_timer_ = EventLoop::kInvalidTimer;
    }

    if (listener_ && listener_->fd() >= 0) {
        if (pg_logger_)
            pg_logger_->notice("{} Listener Disconnected.", conn_tag(*listener_));
        loop_.remove_io(listener_->fd());
    }

    for (auto& c : conns_) {
        if (c->fd() >= 0) {
            if (pg_logger_)
                pg_logger_->notice("{} Disconnected.", conn_tag(*c));
            loop_.remove_io(c->fd());
        }
    }
}

void PgPool::start()
{
    for (std::size_t i = 0; i < min_conns_; ++i)
        new_connection();
}

void PgPool::new_connection()
{
    using namespace std::chrono;

    // Exponential backoff: if the previous connect failed recently, defer
    // this attempt via a one-shot timer instead of looping at event-loop
    // speed.  Without this, an auth fail produces tens of attempts per
    // second, flooding logs and PG's auth subsystem.
    auto now = steady_clock::now();
    if (now < next_connect_attempt_) {
        schedule_reconnect_timer();
        return;
    }

    auto conn = std::make_unique<PgConnection>(conninfo_);
    setup_notice_handler(*conn);

    if (!conn->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: connect_start failed");
        record_connect_failure();
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: CONNECTION_STARTED", conn_tag(*conn));

    PgConnection* raw = conn.get();
    conns_.push_back(std::move(conn));
    registered_conns_.insert(raw);

    loop_.add_io(raw->fd(), EPOLLIN | EPOLLOUT, [this, raw](uint32_t events) {
        on_io(*raw, events);
    });
}

void PgPool::on_io(PgConnection& conn, uint32_t events)
{
    // Remember identity so we can detect whether the handlers below
    // destroyed this connection. replace_connection() erases it from
    // conns_, leaving the caller with a dangling reference; the rearm_io
    // at the end must skip that case.
    const PgConnection* conn_ptr = &conn;

    switch (conn.state()) {

        case PgConnState::Connecting: {
            int old_fd = conn.fd();

            // Use reset_poll for reconnecting connections, connect_poll for new ones
            auto ps = conn.resetting() ? conn.reset_poll() : conn.connect_poll();

            // Handle fd change during handshake (docs: PQsocket can change after each poll)
            int new_fd = conn.fd();
            if (new_fd != old_fd) {
                if (old_fd >= 0)
                    loop_.remove_io(old_fd);
                if (new_fd >= 0) {
                    loop_.add_io(new_fd, EPOLLIN | EPOLLOUT, [this, &conn](uint32_t ev) {
                        on_io(conn, ev);
                    });
                }
            }

            if (pg_logger_)
                pg_logger_->debug("{} {} PollingStatus: {}",
                                 conn_tag(conn),
                                 conn.resetting() ? "Reset" : "Connect",
                                 polling_status_name(ps));

            if (ps == PGRES_POLLING_OK) {
                if (pg_logger_)
                    pg_logger_->notice("{} {}.",
                                      conn_tag(conn),
                                      conn.resetting() ? "Reconnected" : "Connected");
                record_connect_success();
                loop_.modify_io(conn.fd(), EPOLLIN);
                dispatch_queue(conn);
            } else if (ps == PGRES_POLLING_FAILED) {
                if (pg_logger_)
                    pg_logger_->error("{} Connect/Reset failed: {}",
                                     conn_tag(conn), conn.error_message());
                record_connect_failure();
                // Replace this dead connection
                replace_connection(conn);
            } else {
                // Adjust epoll for what poll needs next (Issue #5)
                uint32_t want = (ps == PGRES_POLLING_READING) ? EPOLLIN
                              : (ps == PGRES_POLLING_WRITING) ? EPOLLOUT
                              : (EPOLLIN | EPOLLOUT);
                if (conn.fd() >= 0)
                    loop_.modify_io(conn.fd(), want);
            }
            break;
        }

        case PgConnState::Ready:
            // Drain any async data from the socket (ParameterStatus,
            // NoticeResponse, etc.) so level-triggered EPOLLIN doesn't
            // spin.  Worker connections have no LISTEN subscriptions —
            // NOTIFY is handled by the dedicated listener_ connection.
            if (conn.consume_notify(nullptr) < 0) {
                // The socket is gone and fd() is already -1, so the Error
                // case below can never be reached for this connection:
                // nothing will fire on an fd that no longer exists. Handle
                // it on the spot, the same way that case does.
                //
                // For worker connections this is an acceleration, not a
                // cure — heartbeat() already picks them up by PQstatus
                // within a minute. It is the listener that had no other
                // detector at all.
                if (pg_logger_)
                    pg_logger_->error("{} Connection lost while idle: {}",
                                     conn_tag(conn), conn.error_message());
                fail_inflight_query(conn, "connection lost");
                if (!try_reconnect(conn))
                    replace_connection(conn);
            }
            break;

        case PgConnState::Busy: {
            // Handle pending flush on writable event (Issue #2)
            if ((events & EPOLLOUT) && conn.needs_flush()) {
                if (conn.flush()) {
                    // Fully flushed — only need EPOLLIN for results
                    loop_.modify_io(conn.fd(), EPOLLIN);
                }
                // If flush() returned false (PQflush==1), keep EPOLLOUT
                if (!(events & EPOLLIN))
                    break;  // no readable data yet
            }

            auto results = conn.collect_results();

            // Issue #1: PQconsumeInput failed — connection is dead
            if (conn.state() == PgConnState::Error) {
                if (pg_logger_)
                    pg_logger_->error("{} PQconsumeInput failed: {}",
                                     conn_tag(conn), conn.error_message());
                fail_inflight_query(conn, conn.error_message());
                if (!try_reconnect(conn))
                    replace_connection(conn);
                break;
            }

            if (results.empty()) {
                // collect_results() accumulates partial results internally
                // and only returns them when all statements are complete
                // (state == Ready).  Empty return means either:
                //   a) PQisBusy==1: still waiting for more data.
                //   b) Query produced zero results (edge case) — clear it.
                if (conn.state() == PgConnState::Ready) {
                    const PgQuery* raw_q = conn.current_query();
                    conn.set_current_query(nullptr);
                    if (raw_q) {
                        auto it = std::find_if(inflight_.begin(), inflight_.end(),
                            [raw_q](const std::unique_ptr<PgQuery>& p) {
                                return p.get() == raw_q;
                            });
                        if (it != inflight_.end()) {
                            auto owned = std::move(*it);
                            inflight_.erase(it);
                            if (!owned->canceled())
                                owned->deliver({});
                        }
                    }
                    dispatch_queue(conn);
                }
                break;
            }

            if (pg_logger_) {
                bool quiet = conn.current_query() && conn.current_query()->quiet();
                for (const auto& r : results) {
                    if (r.ok()) {
                        if (!quiet)
                            pg_logger_->debug("{} ResultStatus: {}", conn_tag(conn), r.status_string());
                    } else {
                        pg_logger_->error("{} {}", conn_tag(conn), r.error_message());
                    }
                }
            }

            const PgQuery* raw_q = conn.current_query();
            conn.set_current_query(nullptr);

            if (raw_q) {
                // Find the unique_ptr in inflight_ that owns raw_q
                auto it = std::find_if(inflight_.begin(), inflight_.end(),
                    [raw_q](const std::unique_ptr<PgQuery>& p) {
                        return p.get() == raw_q;
                    });
                if (it != inflight_.end()) {
                    auto owned = std::move(*it);
                    inflight_.erase(it);
                    if (owned->canceled()) {
                        if (pg_logger_)
                            pg_logger_->debug("Discarding results of canceled query {}", owned->id());
                    } else {
                        owned->deliver(std::move(results));
                    }
                }
            }

            dispatch_queue(conn);
            break;
        }

        case PgConnState::Error: {
            if (pg_logger_)
                pg_logger_->error("{} Error: {}", conn_tag(conn), conn.error_message());

            // Fail any in-flight query on this connection
            fail_inflight_query(conn, conn.error_message());

            // Attempt reconnect (mirrors v1 GetReadyConnection -> ResetStart)
            if (!try_reconnect(conn)) {
                // Reconnect failed — replace with a new connection
                replace_connection(conn);
            }
            break;
        }
    }

    // Under APOSTOL_EPOLL_ET the fd was armed with EPOLLONESHOT and the
    // kernel disabled further delivery. Rearm the current mask (set by
    // modify_io inside the switch) so the next read/write transition is
    // signalled. Under LT (flag off) rearm_io is a no-op. registered_conns_
    // lookup is O(1) and survives replace_connection (which erases the
    // pointer from the set before destroying the object), so accessing
    // conn.fd() here is safe only when the set reports still registered.
    if (registered_conns_.count(const_cast<PgConnection*>(conn_ptr))) {
        if (conn.fd() >= 0)
            loop_.rearm_io(conn.fd());
    }
}

bool PgPool::cancel(QueryId id)
{
    if (id == 0)
        return false;

    // Check in-flight queries — send PQcancel to PostgreSQL
    for (auto& conn : conns_) {
        if (conn->current_query() && conn->current_query()->id() == id) {
            conn->current_query()->mark_canceled();
            std::string err;
            bool ok = conn->cancel(err);
            if (pg_logger_) {
                if (ok)
                    pg_logger_->notice("{} Canceled query {}", conn_tag(*conn), id);
                else
                    pg_logger_->error("{} PQcancel failed for query {}: {}",
                                     conn_tag(*conn), id, err);
            }
            return ok;
        }
    }

    // Not in-flight — mark for removal from queue
    canceled_ids_.insert(id);
    return true;
}

void PgPool::dispatch_queue(PgConnection& conn)
{
    // Skip canceled queued queries
    while (!queue_.empty()) {
        auto& front = queue_.front();
        if (canceled_ids_.count(front->id())) {
            if (pg_logger_)
                pg_logger_->debug("Dropping canceled queued query {}", front->id());
            canceled_ids_.erase(front->id());
            queue_.pop();
            continue;
        }
        break;
    }

    if (queue_.empty())
        return;

    // Only dispatch on an idle connection — PQsendQuery requires PQgetResult
    // to have returned NULL.  collect_results() accumulates partial results
    // and only returns them when all statements are complete (state == Ready).
    if (conn.state() != PgConnState::Ready)
        return;

    // Verify connection is actually alive before dispatching
    if (!conn.connected()) {
        if (pg_logger_)
            pg_logger_->error("{} CONNECTION_BAD in dispatch_queue(), reconnecting",
                             conn_tag(conn));
        conn.set_state(PgConnState::Error);
        try_reconnect(conn);
        return;  // queue will be drained when connection recovers
    }

    auto query = std::move(queue_.front());
    queue_.pop();

    PgQuery* raw_q = query.get();
    conn.set_current_query(raw_q);

    if (pg_logger_ && !query->quiet())
        pg_logger_->debug("{} Query: {}", conn_tag(conn), query->sql());

    if (!conn.send_query(query->sql())) {
        if (pg_logger_)
            pg_logger_->error("{} send_query failed in dispatch (state={}): {}, re-queuing",
                             conn_tag(conn),
                             static_cast<int>(conn.state()),
                             conn.error_message());
        conn.set_current_query(nullptr);
        // Re-queue the query instead of losing it (P1 Fix: retry)
        queue_.push(std::move(query));
        conn.set_state(PgConnState::Error);
        try_reconnect(conn);
        return;
    }

    // If flush was incomplete, need EPOLLOUT to continue flushing
    if (conn.needs_flush())
        loop_.modify_io(conn.fd(), EPOLLIN | EPOLLOUT);

    inflight_.push_back(std::move(query));
}

PgPool::QueryId PgPool::execute(std::string              sql,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_exception,
                                bool                      quiet)
{
    auto q = std::make_unique<PgQuery>(std::move(sql), quiet);
    q->on_result(std::move(on_result));
    q->on_exception(std::move(on_exception));

    auto qid = q->id();

    // Find a ready connection (mirrors v1 GetReadyConnection)
    for (auto& c : conns_) {
        if (c->state() == PgConnState::Ready) {
            // P0 Fix: verify actual PQstatus before using (v1 checks Connected())
            if (!c->connected()) {
                if (pg_logger_)
                    pg_logger_->error("{} CONNECTION_BAD detected in execute(), reconnecting",
                                     conn_tag(*c));
                c->set_state(PgConnState::Error);
                try_reconnect(*c);
                continue;  // skip to next connection
            }

            PgQuery* raw_q = q.get();
            c->set_current_query(raw_q);

            if (pg_logger_ && !q->quiet())
                pg_logger_->debug("{} Query: {}", conn_tag(*c), q->sql());

            if (c->send_query(q->sql())) {
                if (c->needs_flush())
                    loop_.modify_io(c->fd(), EPOLLIN | EPOLLOUT);
                inflight_.push_back(std::move(q));
                return qid;
            }

            // send_query failed — connection probably died
            if (pg_logger_)
                pg_logger_->error("{} send_query failed (state={}): {}, reconnecting",
                                 conn_tag(*c),
                                 static_cast<int>(c->state()),
                                 c->error_message());
            c->set_current_query(nullptr);
            c->set_state(PgConnState::Error);
            try_reconnect(*c);
            // Don't return — try next connection or queue
        }
    }

    // No ready connection — queue
    queue_.push(std::move(q));

    // Ensure we have enough connections to eventually drain the queue
    ensure_min_connections();

    return qid;
}

// ── PgPool — Reconnect & Health ───────────────────────────────────────────────

bool PgPool::try_reconnect(PgConnection& conn)
{
    int old_fd = conn.fd();

    // Attempt async reset (reuses existing PGconn handle — cheapest reconnect)
    if (!conn.reset_start()) {
        if (pg_logger_)
            pg_logger_->error("{} reset_start() failed", conn_tag(conn));
        return false;
    }

    int new_fd = conn.fd();

    if (pg_logger_)
        pg_logger_->notice("{} Reconnecting (reset_start)...", conn_tag(conn));

    // PQresetStart() closes the old socket internally, which removes it from
    // epoll.  Even when libpq re-opens a socket with the same fd number, the
    // kernel epoll registration is gone.  Always remove + re-add to be safe.
    if (old_fd >= 0)
        loop_.remove_io(old_fd);
    if (new_fd >= 0) {
        loop_.add_io(new_fd, EPOLLIN | EPOLLOUT, [this, &conn](uint32_t events) {
            on_io(conn, events);
        });
    }

    return true;
}

void PgPool::replace_connection(PgConnection& conn)
{
    int old_fd = conn.fd();
    if (old_fd >= 0)
        loop_.remove_io(old_fd);

    // Drop the pointer from the registered set BEFORE erasing the unique_ptr
    // below (which destroys the object) — otherwise on_io, if it runs the
    // rearm check against a dangling pointer, could observe a false
    // "registered" state.
    registered_conns_.erase(&conn);

    // Find and erase this connection from conns_
    auto it = std::find_if(conns_.begin(), conns_.end(),
        [&conn](const std::unique_ptr<PgConnection>& p) {
            return p.get() == &conn;
        });

    if (it != conns_.end()) {
        if (pg_logger_)
            pg_logger_->notice("{} Replacing dead connection", conn_tag(conn));
        conns_.erase(it);
    }

    // Create replacement
    ensure_min_connections();
}

void PgPool::fail_inflight_query(PgConnection& conn, std::string_view reason)
{
    PgQuery* raw_q = conn.current_query();
    conn.set_current_query(nullptr);

    if (!raw_q)
        return;

    auto it = std::find_if(inflight_.begin(), inflight_.end(),
        [raw_q](const std::unique_ptr<PgQuery>& p) {
            return p.get() == raw_q;
        });

    if (it != inflight_.end()) {
        auto owned = std::move(*it);
        inflight_.erase(it);
        // Re-queue the query for retry instead of failing it permanently.
        //
        // The statement itself is not printed here. This runs at notice — the
        // default level — and the first eighty characters of a query are quite
        // enough to expose a secret: `api.login(E'service-…', E'<secret>'` fits
        // easily. Quiet queries are quiet for a reason, and a connection error is
        // exactly when they get re-queued.
        if (pg_logger_)
            pg_logger_->notice("Re-queuing query {} after connection error: {}",
                               owned->id(), reason);
        queue_.push(std::move(owned));
    }
}

void PgPool::ensure_min_connections()
{
    auto healthy = healthy_count();
    while (healthy < min_conns_ && conns_.size() < max_conns_ + min_conns_) {
        new_connection();
        healthy++;
    }
}

void PgPool::record_connect_success()
{
    consecutive_connect_fails_ = 0;
    next_connect_attempt_      = {};
}

void PgPool::record_connect_failure()
{
    using namespace std::chrono;
    // Already throttled: this failure is a fresh attempt that fired after a
    // previous backoff expired.  Bump the counter, but cap at 6 so the
    // window doesn't grow without bound.  Without the cap, repeated auth
    // failures would push the next attempt centuries into the future.
    if (consecutive_connect_fails_ < 6)
        consecutive_connect_fails_++;
    int delay_sec = 1 << (consecutive_connect_fails_ - 1);  // 1, 2, 4, 8, 16, 32, 64
    if (delay_sec > 60) delay_sec = 60;
    next_connect_attempt_ = steady_clock::now() + seconds(delay_sec);
}

void PgPool::schedule_reconnect_timer()
{
    using namespace std::chrono;

    if (reconnect_timer_ != EventLoop::kInvalidTimer)
        return;  // already armed — its callback below covers both concerns

    auto now = steady_clock::now();
    // Floor to 1ms even when next_connect_attempt_ is still (barely) ahead of
    // now: a computed 0ms duration disarms timerfd instead of firing it ASAP
    // (POSIX itimerspec semantics — an all-zero it_value disarms regardless
    // of it_interval), which would leave reconnect_timer_ permanently
    // "armed" on a timer that never fires — silently stalling every future
    // reconnect attempt (pool and listener alike) behind schedule_reconnect_
    // timer()'s already-armed guard.
    auto delay = duration_cast<milliseconds>(next_connect_attempt_ - now);
    if (delay.count() < 1) delay = milliseconds(1);

    if (pg_logger_)
        pg_logger_->notice(
            "PgPool: reconnect throttled (consecutive fails={}) — next attempt in {}ms",
            consecutive_connect_fails_, delay.count());

    reconnect_timer_ = loop_.add_timer(delay,
        [this] {
            reconnect_timer_ = EventLoop::kInvalidTimer;
            ensure_min_connections();
            // listener_ may have died on the same outage that triggered this
            // timer (or failed independently at startup, see start_listener());
            // restart it if there's still something to LISTEN for.
            if (!listener_ && !notify_handlers_.empty())
                start_listener();
        },
        /*repeat=*/false);
}

std::size_t PgPool::outstanding() const
{
    std::size_t n = queue_.size();

    for (const auto& conn : conns_)
        if (conn && conn->current_query())
            ++n;

    return n;
}

std::size_t PgPool::healthy_count() const
{
    std::size_t count = 0;
    for (const auto& c : conns_) {
        auto s = c->state();
        if ((s == PgConnState::Ready || s == PgConnState::Busy || s == PgConnState::Connecting)
            && c->fd() >= 0)
            count++;
    }
    return count;
}

void PgPool::heartbeat()
{
    // Check each connection's actual PQstatus (not cached state_)
    for (auto& c : conns_) {
        if (c->state() == PgConnState::Ready || c->state() == PgConnState::Busy) {
            if (!c->connected()) {
                if (pg_logger_)
                    pg_logger_->error("{} CONNECTION_BAD detected in heartbeat, reconnecting",
                                     conn_tag(*c));
                fail_inflight_query(*c, "connection lost");
                if (!try_reconnect(*c)) {
                    replace_connection(*c);
                    break;  // conns_ was modified, restart on next heartbeat
                }
            }
        }
    }

    // Ensure min_conns
    ensure_min_connections();

    // Drain queue if there are ready connections
    for (auto& c : conns_) {
        if (c->state() == PgConnState::Ready && !queue_.empty())
            dispatch_queue(*c);
    }

    // The listener lives outside conns_, so the loop above never saw it — and
    // everything else that could notice its death is event-driven. When
    // PQconsumeInput fails libpq closes the socket itself, after which NO
    // epoll event can arrive, ever. A timer is therefore the only thing that
    // can observe the loss: this is the guarantee, and the return-value check
    // in consume_notify() is only the fast path to it.
    //
    // It matters most where nobody is watching. A channel that is quiet by
    // design (a vessel at sea) produces no events to ride on, and "no
    // notifications" is indistinguishable from "no subscription".
    if (listener_) {
        if (listener_is_dead())
            restart_listener("heartbeat found the listener dead");
    } else if (!notify_handlers_.empty()) {
        // Subscriptions outstanding and no listener object at all.
        if (pg_logger_)
            pg_logger_->error("PgPool: {} LISTEN channel(s) with no listener — restarting",
                             notify_handlers_.size());
        start_listener();
    }
}

// ── PgPool — LISTEN / NOTIFY ──────────────────────────────────────────────────

void PgPool::listen(const std::string& channel, NotifyHandler cb)
{
    bool is_new_channel = !notify_handlers_.contains(channel);
    notify_handlers_[channel].push_back(std::move(cb));

    if (is_new_channel)
        pending_listens_.insert(channel);

    if (!listener_) {
        start_listener();
    } else if (listener_is_dead()) {
        // A listener object outlives its socket: libpq closes the fd and keeps
        // the PGconn. Without this branch the pair below was a lock — the
        // first test saw a non-null listener_ and said nothing, the second
        // shipped LISTEN into a corpse — and listen() is only ever called
        // from on_start(), so the subscription was lost until the process was
        // restarted by hand.
        //
        // Liveness is asked of PQstatus (connected()), never of fd(): fd() is
        // cached at connect time and still reports the old number after libpq
        // closes the socket. Connecting is excluded — a handshake in flight is
        // not yet connected() and must not be torn down.
        restart_listener("listen() found the listener dead");
    } else if (listener_->state() == PgConnState::Ready && !pending_listens_.empty()) {
        send_pending_listens();
    }
    // else: connecting or busy — pending_listens_ will be picked up on next state change
}

void PgPool::unlisten(std::string_view channel)
{
    std::string ch(channel);
    notify_handlers_.erase(ch);
    pending_listens_.erase(ch);

    if (listener_ && listener_->state() == PgConnState::Ready)
        listener_->send_query("UNLISTEN \"" + ch + "\"");
    // If listener is busy/connecting, callbacks are already removed from
    // notify_handlers_ so notifications won't be dispatched even if UNLISTEN
    // hasn't been sent yet.
}

void PgPool::start_listener()
{
    using namespace std::chrono;

    // Honour the same backoff window as the regular pool — an unreachable
    // postgres/pgbouncer shouldn't be hammered by a second, independent
    // retry loop running at full event-loop speed on top of the pool's own.
    if (steady_clock::now() < next_connect_attempt_) {
        schedule_reconnect_timer();
        return;
    }

    listener_ = std::make_unique<PgConnection>(conninfo_);
    setup_notice_handler(*listener_);

    if (!listener_->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: listener connect_start failed");
        listener_.reset();
        // Without this, a listener that fails synchronously at startup (e.g.
        // pgbouncer/postgres not yet accepting) is abandoned forever: nothing
        // else ever calls start_listener() again for it, since the one-shot
        // init_listen() caller only retries on ITS OWN query failing, not on
        // the listener connection specifically.
        record_connect_failure();
        schedule_reconnect_timer();
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: LISTENER CONNECTION_STARTED", conn_tag(*listener_));

    int fd = listener_->fd();
    loop_.add_io(fd, EPOLLIN | EPOLLOUT, [this](uint32_t events) {
        on_listener_io(events);
    });
}

bool PgPool::listener_is_dead() const
{
    if (!listener_)
        return false;
    if (listener_->state() == PgConnState::Error)
        return true;
    // A handshake in flight is not connected() yet and must not be torn down.
    if (listener_->state() == PgConnState::Connecting)
        return false;
    return listener_->fd() < 0 || !listener_->connected();
}

void PgPool::restart_listener(std::string_view reason)
{
    // Copy first: `reason` is usually PQerrorMessage() pointing into the
    // PGconn we are about to destroy.
    const std::string why(reason);
    const std::size_t channels = notify_handlers_.size();

    if (listener_) {
        // conn_tag() carries the fd, so a dead listener logs fd=-1 — the
        // visible proof that libpq closed the socket under us and that no
        // epoll event could have followed.
        if (pg_logger_) {
            // conn_tag() prints the CURRENT fd, which is -1 once the socket is
            // gone. "-1" is true and unusable: this very defect was caught
            // because a real number turned up in a log line. Print both — the
            // number that was, and the fact that it no longer is.
            const int fd_now = listener_->fd();
            pg_logger_->error("{} LISTEN subscription lost ({}) — socket {} — restoring {} channel(s)",
                             conn_tag(*listener_), why,
                             fd_now >= 0 ? fmt::format("fd={}", fd_now)
                                         : fmt::format("fd={} gone", listener_->last_fd()),
                             channels);
        }
        // fd() is trustworthy here only because collect_results() and
        // consume_notify() now re-read PQsocket the moment they find the
        // socket gone. Before that it was a stale cache — measured: the loss
        // line logged fd=22 for a socket libpq had already closed — and
        // deregistering a stale number removes whichever connection has since
        // been handed that fd. The pools share one EventLoop, so the victim
        // would be some other connection going quietly deaf, nowhere near
        // here. Fixing the cache at the source keeps every `fd() >= 0` test
        // in this file honest, including the two outside this function.
        if (listener_->fd() >= 0)
            loop_.remove_io(listener_->fd());
        listener_.reset();
    } else if (pg_logger_) {
        pg_logger_->error("PgPool: LISTEN subscription lost ({}) — restoring {} channel(s)",
                         why, channels);
    }

    // Re-arm from the REGISTRY, never from a literal list of channels.
    // WebSocketAPI subscribes to channels it reads from the database at
    // runtime (daemon.publisher_list()); enumerating channels statically here
    // would silently drop every one of them and take the whole dashboard
    // publication with it, while a test on the compiled-in channels still
    // passed.
    for (const auto& [ch, _] : notify_handlers_)
        pending_listens_.insert(ch);

    // Safe to call unconditionally (no hot-loop risk): start_listener() itself
    // checks next_connect_attempt_ and defers via schedule_reconnect_timer()
    // when a connect attempt would be premature or fails again.
    if (!notify_handlers_.empty())
        start_listener();
}

void PgPool::send_pending_listens()
{
    if (pending_listens_.empty() || !listener_)
        return;

    std::string sql;
    std::string names;
    for (const auto& ch : pending_listens_) {
        sql += "LISTEN \"" + ch + "\";";
        if (!names.empty())
            names += ", ";
        names += ch;
    }

    // Until now the word LISTEN never appeared in the log at all. The only
    // external sign of a live subscription was the ASYNC NOTIFY line, and
    // that says notifications ARRIVE — not that the subscription EXISTS.
    // The two differ precisely when it matters: on a quiet channel.
    if (pg_logger_)
        pg_logger_->notice("{} LISTEN {}", conn_tag(*listener_), names);

    auto sent = std::move(pending_listens_);
    pending_listens_.clear();

    if (!listener_->send_query(sql)) {  // listener → Busy
        // The channels were cleared above. Losing them here would leave a
        // healthy listener subscribed to nothing — the same silent outcome
        // by a different route.
        pending_listens_ = std::move(sent);
        restart_listener("send_query failed while shipping LISTEN");
    }
}

void PgPool::on_listener_io(uint32_t events)
{
    if (!listener_) return;

    // EPOLLERR/EPOLLHUP on the listener connection = libpq socket is gone.
    // Transition to Error state so the Error branch below reconnects from
    // scratch; without this the listener would silently loop rearming a
    // dead fd under LT, or stay disarmed under ET.
    if (events & (EPOLLERR | EPOLLHUP))
        listener_->set_state(PgConnState::Error);

    switch (listener_->state()) {

        case PgConnState::Connecting: {
            auto ps = listener_->connect_poll();

            if (pg_logger_)
                pg_logger_->debug("{} Listener PollingStatus: {}", conn_tag(*listener_), polling_status_name(ps));

            if (ps == PGRES_POLLING_OK) {
                if (pg_logger_)
                    pg_logger_->notice("{} Listener Connected.", conn_tag(*listener_));
                loop_.modify_io(listener_->fd(), EPOLLIN);
                if (!pending_listens_.empty())
                    send_pending_listens();
            } else if (ps == PGRES_POLLING_FAILED) {
                if (pg_logger_)
                    pg_logger_->error("{} Listener Error: {}", conn_tag(*listener_), listener_->error_message());
                loop_.remove_io(listener_->fd());
                listener_.reset();
                // pending_listens_ already holds every channel (it's only
                // cleared once send_pending_listens() actually ships a LISTEN,
                // which never happened here) — just get a retry scheduled.
                record_connect_failure();
                schedule_reconnect_timer();
            } else {
                // Adjust epoll for what poll needs next (same as worker connections)
                uint32_t want = (ps == PGRES_POLLING_READING) ? EPOLLIN
                              : (ps == PGRES_POLLING_WRITING) ? EPOLLOUT
                              : (EPOLLIN | EPOLLOUT);
                if (listener_->fd() >= 0)
                    loop_.modify_io(listener_->fd(), want);
            }
            break;
        }

        case PgConnState::Busy: {
            // Waiting for LISTEN/UNLISTEN command response
            auto results = listener_->collect_results();
            if (results.empty()) {
                // collect_results() may have found the socket dead and moved
                // the listener to Error before returning nothing. Plain
                // `break` dropped that discovery on the floor: the rearm at
                // the bottom is gated on state != Error, so nothing was left
                // to drive the Error case — the failure was detected and
                // thrown away.
                if (listener_->state() == PgConnState::Error) {
                    restart_listener("PQconsumeInput failed while awaiting LISTEN");
                    return;
                }
                break;  // still in progress
            }

            bool all_ok = true;
            for (const auto& r : results) {
                if (!r.ok())
                    all_ok = false;
                if (!pg_logger_)
                    continue;
                if (r.ok())
                    pg_logger_->debug("{} Listener ResultStatus: {}", conn_tag(*listener_), r.status_string());
                else
                    pg_logger_->error("{} Listener {}", conn_tag(*listener_), r.error_message());
            }

            // Command completed (LISTEN/UNLISTEN returned PGRES_COMMAND_OK)
            // Send any further pending listens, or transition to Ready
            if (!pending_listens_.empty()) {
                // send_pending_listens() can fail its send_query, call
                // restart_listener(), and leave listener_ EMPTY: start_listener()
                // returns without creating one inside the backoff window, and
                // resets it when connect_start() fails. Comparing identity
                // rather than testing for null covers the other half too — on a
                // successful restart listener_ is a DIFFERENT connection, still
                // in Connecting, and calling consume_notify on it would at best
                // do nothing and at worst tear down the replacement we just made.
                auto* before = listener_.get();
                send_pending_listens();   // → Busy again
                if (listener_.get() != before)
                    return;               // listener replaced or gone — the tail is not ours
            } else if (all_ok && pg_logger_) {
                // The confirmation half of the pair: the command was shipped
                // AND the server acknowledged it. This line is what tells an
                // operator that a silent channel is subscribed rather than
                // orphaned. Worded for both commands that land here — after an
                // UNLISTEN "established" would name the wrong event, while the
                // count stays right either way.
                pg_logger_->notice("{} LISTEN active on {} channel(s)",
                                  conn_tag(*listener_), notify_handlers_.size());
            }
            // In either case, check for notifications that arrived simultaneously
            if (listener_->consume_notify([this](const char* ch, const char* payload) {
                    dispatch_notify(ch, payload);
                }) < 0)
            {
                restart_listener("PQconsumeInput failed right after LISTEN");
                return;
            }
            break;
        }

        case PgConnState::Ready: {
            // EPOLLIN: incoming notification — or the FIN that kills the
            // subscription. This is the one moment the death is observable:
            // the fd dies INSIDE this call, and there is no second event.
            if (listener_->consume_notify([this](const char* ch, const char* payload) {
                    dispatch_notify(ch, payload);
                }) < 0)
            {
                restart_listener("PQconsumeInput failed on an idle listener");
                return;
            }
            // Send any pending listens accumulated while we were processing
            if (!pending_listens_.empty())
                send_pending_listens();
            break;
        }

        case PgConnState::Error:
            restart_listener(listener_->error_message());
            // start_listener() registered the replacement fd via add_io,
            // which arms it; the old fd is gone. Skip the rearm below rather
            // than touch a connection this handler just replaced.
            return;
    }

    // Under APOSTOL_EPOLL_ET every fd is armed with EPOLLONESHOT and must be
    // rearmed after each event. Skip when the handler destroyed or replaced
    // the listener (Error path calls listener_.reset(); start_listener()
    // registers the new fd via add_io, which arms it on its own).
    if (listener_ && listener_->fd() >= 0 &&
        listener_->state() != PgConnState::Error)
    {
        loop_.rearm_io(listener_->fd());
    }
}

void PgPool::dispatch_notify(const char* channel, const char* payload)
{
    if (pg_logger_ && listener_) {
        pg_logger_->notice("{} ASYNC NOTIFY of '{}' received from backend PID {}",
            conn_tag(*listener_), channel, listener_->backend_pid());
    }

    auto it = notify_handlers_.find(channel);
    if (it == notify_handlers_.end()) return;

    // Copy the handler list — callbacks may mutate notify_handlers_ via unlisten()
    auto handlers = it->second;
    for (const auto& cb : handlers)
        cb(channel, payload ? payload : "");
}

} // namespace apostol
