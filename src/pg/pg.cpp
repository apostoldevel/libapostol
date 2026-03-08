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

    fd_ = PQsocket(raw);
    resetting_ = false;
    return true;
}

PostgresPollingStatusType PgConnection::connect_poll()
{
    auto ps = PQconnectPoll(conn_.get());
    fd_ = PQsocket(conn_.get());  // docs: re-determine socket after each poll

    if (ps == PGRES_POLLING_OK)
        state_ = PgConnState::Ready;
    else if (ps == PGRES_POLLING_FAILED)
        state_ = PgConnState::Error;

    return ps;
}

bool PgConnection::reset_start()
{
    if (!PQresetStart(conn_.get())) {
        state_ = PgConnState::Error;
        return false;
    }
    PQsetnonblocking(conn_.get(), 1);  // restore nonblocking after reset
    state_ = PgConnState::Connecting;
    resetting_ = true;
    fd_    = PQsocket(conn_.get());
    return true;
}

PostgresPollingStatusType PgConnection::reset_poll()
{
    auto ps = PQresetPoll(conn_.get());
    fd_ = PQsocket(conn_.get());  // docs: re-determine socket after each poll

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
    std::vector<PgResult> out;

    if (PQconsumeInput(conn_.get()) == 0) {
        state_ = PgConnState::Error;  // connection is dead
        return out;
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
        out.emplace_back(r);
    }
    // If loop exited via PQisBusy==1, state stays Busy — the trailing NULL
    // (ReadyForQuery) hasn't arrived yet.  Results in `out` are complete and
    // safe to deliver; the next EPOLLIN will consume the NULL and set Ready.

    return out;
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
    PQconsumeInput(conn_.get());
    int count = 0;
    PGnotify* notify;
    while ((notify = PQnotifies(conn_.get())) != nullptr) {
        if (cb)
            cb(notify->relname, notify->extra);
        PQfreemem(notify);
        ++count;
        PQconsumeInput(conn_.get());
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
    auto conn = std::make_unique<PgConnection>(conninfo_);
    setup_notice_handler(*conn);

    if (!conn->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: connect_start failed");
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: CONNECTION_STARTED", conn_tag(*conn));

    PgConnection* raw = conn.get();
    conns_.push_back(std::move(conn));

    loop_.add_io(raw->fd(), EPOLLIN | EPOLLOUT, [this, raw](uint32_t events) {
        on_io(*raw, events);
    });
}

void PgPool::on_io(PgConnection& conn, uint32_t events)
{
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
                loop_.modify_io(conn.fd(), EPOLLIN);
                dispatch_queue(conn);
            } else if (ps == PGRES_POLLING_FAILED) {
                if (pg_logger_)
                    pg_logger_->error("{} Connect/Reset failed: {}",
                                     conn_tag(conn), conn.error_message());
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
            // Worker connections have no LISTEN subscriptions —
            // NOTIFY is handled by the dedicated listener_ connection.
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
                // No new result rows.  Two sub-cases:
                // a) PQisBusy==1: still waiting for data → nothing to do.
                // b) PQgetResult returned NULL after a previous split-TCP
                //    delivery → state is now Ready, drain the queue.
                if (conn.state() == PgConnState::Ready)
                    dispatch_queue(conn);
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
    // to have returned NULL.  After a split-TCP collect_results() the conn may
    // still be Busy (results delivered, but trailing NULL not yet consumed).
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
        // Re-queue the query for retry instead of failing it permanently
        if (pg_logger_)
            pg_logger_->notice("Re-queuing query after connection error: {}",
                              owned->sql().substr(0, 80));
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
    listener_ = std::make_unique<PgConnection>(conninfo_);
    setup_notice_handler(*listener_);

    if (!listener_->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: listener connect_start failed");
        listener_.reset();
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: LISTENER CONNECTION_STARTED", conn_tag(*listener_));

    int fd = listener_->fd();
    loop_.add_io(fd, EPOLLIN | EPOLLOUT, [this](uint32_t events) {
        on_listener_io(events);
    });
}

void PgPool::send_pending_listens()
{
    if (pending_listens_.empty() || !listener_)
        return;

    std::string sql;
    for (const auto& ch : pending_listens_)
        sql += "LISTEN \"" + ch + "\";";
    pending_listens_.clear();

    listener_->send_query(sql);  // listener → Busy
}

void PgPool::on_listener_io(uint32_t /*events*/)
{
    if (!listener_) return;

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
            }
            break;
        }

        case PgConnState::Busy: {
            // Waiting for LISTEN/UNLISTEN command response
            auto results = listener_->collect_results();
            if (results.empty())
                break;  // still in progress

            if (pg_logger_) {
                for (const auto& r : results) {
                    if (r.ok())
                        pg_logger_->debug("{} Listener ResultStatus: {}", conn_tag(*listener_), r.status_string());
                    else
                        pg_logger_->error("{} Listener {}", conn_tag(*listener_), r.error_message());
                }
            }

            // Command completed (LISTEN/UNLISTEN returned PGRES_COMMAND_OK)
            // Send any further pending listens, or transition to Ready
            if (!pending_listens_.empty()) {
                send_pending_listens();   // → Busy again
            }
            // In either case, check for notifications that arrived simultaneously
            listener_->consume_notify([this](const char* ch, const char* payload) {
                dispatch_notify(ch, payload);
            });
            break;
        }

        case PgConnState::Ready: {
            // EPOLLIN: incoming notification
            listener_->consume_notify([this](const char* ch, const char* payload) {
                dispatch_notify(ch, payload);
            });
            // Send any pending listens accumulated while we were processing
            if (!pending_listens_.empty())
                send_pending_listens();
            break;
        }

        case PgConnState::Error:
            if (pg_logger_)
                pg_logger_->error("{} Listener Error: {}", conn_tag(*listener_), listener_->error_message());
            loop_.remove_io(listener_->fd());
            listener_.reset();
            // Re-populate pending channels so they get re-LISTENed on reconnect
            for (const auto& [ch, _] : notify_handlers_)
                pending_listens_.insert(ch);
            if (!notify_handlers_.empty())
                start_listener();
            break;
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
