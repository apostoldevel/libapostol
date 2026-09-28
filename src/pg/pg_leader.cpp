#include "apostol/pg_leader.hpp"

#include "apostol/logger.hpp"
#include "apostol/pg.hpp"
#include "apostol/pg_utils.hpp"

#include <sys/epoll.h>

#include <algorithm>

#include <fmt/format.h>

namespace apostol
{

namespace
{

constexpr std::chrono::seconds k_min_answer_timeout{15};

// libpq writes several lines ("server closed the connection unexpectedly\n\t
// This probably means…"); a log line is one line, so whitespace runs become one
// space.
std::string one_line(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (!space)
            out += c;
        else if (!out.empty() && out.back() != ' ')
            out += ' ';
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace

// ─── PgLeaderLock ────────────────────────────────────────────────────────────

std::int64_t PgLeaderLock::key_hash(std::string_view key) noexcept
{
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return static_cast<std::int64_t>(h);
}

PgLeaderLock::PgLeaderLock(EventLoop& loop, std::string conninfo, std::string key,
                           std::string holder, std::chrono::seconds interval,
                           Logger* logger)
    : loop_(loop)
    , conninfo_(std::move(conninfo))
    , key_(std::move(key))
    , holder_(std::move(holder))
    , interval_(std::max(interval, std::chrono::seconds(1)))
    , logger_(logger)
    , lock_id_(key_hash(key_))
{
}

PgLeaderLock::~PgLeaderLock()
{
    if (timer_ != EventLoop::kInvalidTimer)
        loop_.cancel_timer(timer_);

    const bool was_held = held_;
    close();   // PQfinish: the server ends the session and the lock with it

    if (was_held && logger_)
        logger_->notice("{} released", tag());
}

std::chrono::seconds PgLeaderLock::answer_timeout() const noexcept
{
    return std::max(interval_ * 3, k_min_answer_timeout);
}

std::chrono::seconds PgLeaderLock::server_timeout() const noexcept
{
    return answer_timeout() * 4;
}

bool PgLeaderLock::fresh() const noexcept
{
    return held_ && std::chrono::steady_clock::now() - confirmed_ <= answer_timeout();
}

std::string PgLeaderLock::tag() const
{
    return fmt::format("leader '{}'", key_);
}

void PgLeaderLock::start(AcquiredHandler on_acquired, LostHandler on_lost)
{
    on_acquired_ = std::move(on_acquired);
    on_lost_     = std::move(on_lost);

    if (logger_)
        logger_->notice("{} asking for the role as '{}' (lock {}, every {}s)",
                        tag(), holder_, lock_id_, interval_.count());

    timer_ = loop_.add_timer(interval_, [this] { tick(); });
    connect();
}

void PgLeaderLock::tick()
{
    if (inert_)
        return;

    if (!conn_) {
        connect();
        return;
    }

    const bool overdue = std::chrono::steady_clock::now() - since_ > answer_timeout();

    switch (conn_->state()) {
        case PgConnState::Connecting:
            if (overdue)
                drop(fmt::format("no connection in {}s", answer_timeout().count()));
            return;

        case PgConnState::Busy:
            if (!overdue)
                return;
            // A loop that was held up elsewhere may reach this timer before the
            // socket event that carries the answer. Read before judging, or a
            // slow tick on a healthy connection would cost the leadership.
            on_io(EPOLLIN);
            if (conn_ && conn_->state() == PgConnState::Busy)
                drop(fmt::format("no answer from the server in {}s", answer_timeout().count()));
            return;

        case PgConnState::Ready:
            send(held_ ? Ask::probe : Ask::try_lock);
            return;

        case PgConnState::Error:
            drop(conn_->error_message());
            return;
    }
}

void PgLeaderLock::connect()
{
    conn_   = std::make_unique<PgConnection>(conninfo_);
    since_  = std::chrono::steady_clock::now();
    set_up_ = false;
    asked_  = Ask::none;

    // A FATAL on an idle session (idle_session_timeout, pg_terminate_backend)
    // reaches libpq as a notice; without a processor it went to stderr and the
    // loss line said only "server closed the connection unexpectedly".
    conn_->set_notice_callback([this](const char* message) {
        if (logger_)
            logger_->warn("{} server: {}", tag(), one_line(message));
    });

    if (!conn_->connect_start()) {
        const std::string why = fmt::format("connect failed: {}", conn_->error_message());
        conn_.reset();   // never registered with the loop
        drop(why);
        return;
    }

    loop_.add_io(conn_->fd(), EPOLLIN | EPOLLOUT, [this](std::uint32_t events) {
        on_io(events);
    });
}

void PgLeaderLock::on_io(std::uint32_t events)
{
    if (!conn_ || inert_)
        return;

    auto* const before = conn_.get();

    // During the handshake an error event is libpq's to judge, not ours: a
    // refused connect reports EPOLLERR|EPOLLHUP, and connect_poll() is what
    // moves on to the next address or host (host=pg1,pg2; localhost as ::1
    // then 127.0.0.1). Dropping here left the role unheld on every node.
    if ((events & (EPOLLERR | EPOLLHUP)) && conn_->state() != PgConnState::Connecting) {
        // Read what the server said last — "terminating connection due to
        // idle-session timeout" is worth more in the log than a bare hangup.
        (void)conn_->collect_results();
        const std::string msg = conn_->error_message();
        drop(msg.empty() ? std::string("connection closed") : msg);
        return;
    }

    switch (conn_->state()) {
        case PgConnState::Connecting: {
            // libpq may close the socket and open another inside one poll —
            // next host, target_session_attrs, sslmode=prefer falling back —
            // and the new one often gets the same number. epoll dropped the
            // old registration with the close, so MOD on "the same" fd fails
            // with ENOENT and throws out of the loop. Take the fd out before
            // polling and register whatever is there after.
            if (conn_->fd() >= 0)
                loop_.remove_io(conn_->fd());

            const auto ps = conn_->connect_poll();

            if (ps == PGRES_POLLING_FAILED) {
                drop(conn_->error_message());
                return;
            }

            const std::uint32_t want = ps == PGRES_POLLING_OK      ? EPOLLIN
                                     : ps == PGRES_POLLING_READING ? EPOLLIN
                                     : ps == PGRES_POLLING_WRITING ? EPOLLOUT
                                                                   : (EPOLLIN | EPOLLOUT);
            if (conn_->fd() < 0) {
                drop(conn_->error_message());
                return;
            }
            loop_.add_io(conn_->fd(), want, [this](std::uint32_t ev) { on_io(ev); });

            if (ps == PGRES_POLLING_OK)
                send(Ask::try_lock);
            return;   // add_io armed the fd; nothing to rearm
        }

        case PgConnState::Busy: {
            if ((events & EPOLLOUT) && conn_->needs_flush() && conn_->flush())
                loop_.modify_io(conn_->fd(), EPOLLIN);

            auto results = conn_->collect_results();
            if (results.empty()) {
                if (conn_->state() == PgConnState::Error) {
                    drop(conn_->error_message());
                    return;
                }
                break;   // the answer is not complete yet
            }
            handle(results);
            break;
        }

        case PgConnState::Ready:
            // Nothing asked: a readable idle socket is the server closing it.
            if (conn_->consume_notify([](const char*, const char*) {}) < 0) {
                drop(conn_->error_message());
                return;
            }
            break;

        case PgConnState::Error:
            drop(conn_->error_message());
            return;
    }

    // Under APOSTOL_EPOLL_ET the fd is one-shot. Not when a handler above
    // closed or replaced the connection — its fd is not ours to touch.
    if (conn_ && conn_.get() == before && conn_->fd() >= 0 &&
        conn_->state() != PgConnState::Error)
    {
        loop_.rearm_io(conn_->fd());
    }
}

void PgLeaderLock::send(Ask what)
{
    const auto id = static_cast<std::uint64_t>(lock_id_);
    const auto hi = static_cast<std::uint32_t>(id >> 32);
    const auto lo = static_cast<std::uint32_t>(id);

    // pg_locks shows a bigint advisory key as classid (high half), objid (low
    // half) and objsubid 1.
    const std::string where = fmt::format(
        "l.locktype = 'advisory' AND l.classid = '{}'::oid AND l.objid = '{}'::oid "
        "AND l.objsubid = 1 AND l.granted", hi, lo);

    std::string sql;

    if (!set_up_) {
        // idle_session_timeout exists from PostgreSQL 14; on an older server
        // the WHERE is false and set_config() is never called.
        sql = fmt::format(
            "SET application_name = {}; "
            "SELECT set_config('idle_session_timeout', '{}s', false) "
            "WHERE current_setting('server_version_num')::int >= 140000; ",
            pq_quote_literal(holder_), server_timeout().count());
        set_up_ = true;
    }

    if (what == Ask::try_lock) {
        sql += fmt::format(
            "SELECT pg_try_advisory_lock({}), "
            "(SELECT a.application_name FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid "
            "WHERE {} AND l.pid <> pg_backend_pid() "
            "AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database()) "
            "LIMIT 1), pg_backend_pid()",
            lock_id_, where);
    } else {
        sql += fmt::format(
            "SELECT EXISTS (SELECT FROM pg_locks l WHERE {} AND l.pid = pg_backend_pid()), "
            "pg_backend_pid()",
            where);
    }

    if (!conn_->send_query(sql)) {
        drop(fmt::format("cannot send the query: {}", conn_->error_message()));
        return;
    }

    asked_ = what;
    since_ = std::chrono::steady_clock::now();

    if (conn_->needs_flush() && conn_->fd() >= 0)
        loop_.modify_io(conn_->fd(), EPOLLIN | EPOLLOUT);
}

void PgLeaderLock::handle(std::vector<PgResult>& results)
{
    const Ask what = asked_;
    asked_ = Ask::none;

    for (const auto& r : results) {
        if (!r.ok()) {
            drop(fmt::format("the server refused: {}", r.error_message()));
            return;
        }
    }

    const auto& last = results.back();
    if (last.rows() < 1 || last.columns() < 1 || last.is_null(0, 0)) {
        drop("unexpected answer from the server");
        return;
    }

    const bool yes = last.value(0, 0)[0] == 't';

    // One session answers every question, or the lock means nothing: behind
    // transaction pooling consecutive queries land on different server
    // connections. Named here, instead of surfacing as a lock found missing.
    const int pid_col = last.columns() - 1;
    if (pid_col > 0 && !last.is_null(0, pid_col)) {
        const std::string pid = last.value(0, pid_col);
        if (session_pid_.empty()) {
            session_pid_ = pid;
        } else if (pid != session_pid_) {
            drop(fmt::format("the server session changed ({} → {}): the lock connection is pooled "
                             "per transaction, and a session lock needs a session", session_pid_, pid));
            return;
        }
    }

    if (what == Ask::probe) {
        if (!yes)
            lose("the session is alive but no longer holds the lock "
                 "(transaction pooling between here and PostgreSQL?)");
        else
            confirmed_ = std::chrono::steady_clock::now();
        return;
    }

    if (yes) {
        acquired();
        return;
    }

    std::string holder;
    if (last.columns() > 1 && !last.is_null(0, 1))
        holder = last.value(0, 1);

    last_error_.clear();   // connected and answered: a new failure is news again

    if (!waiting_logged_ || holder != last_holder_) {
        if (logger_)
            logger_->notice("{} standby: the role is held by {} — asking again every {}s",
                            tag(),
                            holder.empty() ? std::string("another session")
                                           : fmt::format("'{}'", holder),
                            interval_.count());
        waiting_logged_ = true;
        last_holder_    = std::move(holder);
    }
}

void PgLeaderLock::acquired()
{
    held_      = true;
    confirmed_ = std::chrono::steady_clock::now();

    if (logger_)
        logger_->notice("{} acquired — this process is the leader (lock {}, backend pid {})",
                        tag(), lock_id_, conn_ ? conn_->backend_pid() : 0);

    if (on_acquired_)
        on_acquired_();
}

void PgLeaderLock::drop(std::string_view why)
{
    // `why` often points into the PGconn that close() is about to free.
    std::string reason = one_line(why);

    if (held_) {
        lose(reason);
        return;
    }

    close();

    // A standby on an unreachable server asks every few seconds; one line per
    // distinct failure, not one per attempt.
    if (logger_) {
        if (reason != last_error_)
            logger_->warn("{} standby: {} — retrying every {}s", tag(), reason, interval_.count());
        else
            logger_->debug("{} standby: {}", tag(), reason);
    }
    last_error_ = std::move(reason);
}

void PgLeaderLock::lose(std::string_view why)
{
    const std::string reason(why);

    held_  = false;
    inert_ = true;

    if (timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(timer_);
        timer_ = EventLoop::kInvalidTimer;
    }

    // Out of the loop, but NOT closed. The caller now stops its modules and
    // drains, which takes a while; a session that is merely slow still holds
    // the lock, and PQfinish here would hand the role to a standby while this
    // process is still working. The destructor closes it — after the work.
    if (conn_ && conn_->fd() >= 0)
        loop_.remove_io(conn_->fd());

    if (logger_)
        logger_->error("{} LOST: {}", tag(), reason);

    if (on_lost_)
        on_lost_(reason);
}

void PgLeaderLock::close()
{
    if (conn_) {
        if (conn_->fd() >= 0)
            loop_.remove_io(conn_->fd());
        conn_.reset();
    }
    asked_  = Ask::none;
    set_up_ = false;
    session_pid_.clear();
}

} // namespace apostol
