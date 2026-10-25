#include "apostol/pg.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace apostol
{

namespace
{

// The text a failed statement is reported with, on one line.
//
// PQresultErrorMessage ends in a newline, and for a status that carries no
// message of its own it is empty — so fall back to the status name rather than
// handing the caller an empty string it can neither log nor test.
//
// The interior is folded onto one line too, which matters more than it looks: a
// PL/pgSQL failure carries CONTEXT lines,
//
//     ERROR:  division by zero
//     CONTEXT:  SQL statement "SELECT 1/0"
//     PL/pgSQL function inline_code_block line 1 at PERFORM
//
// and this text does not only go to a log. TaskScheduler, ReportServer and
// MessageServer pass it to api.set_object_label as the object's label, where a
// three-line value is read back by people and by screens that expect one line.
std::string result_error_text(const PgResult& r)
{
    const char* raw = r.error_message();
    std::string msg = raw ? raw : "";

    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };

    std::string one_line;
    one_line.reserve(msg.size());
    bool pending_space = false;
    for (unsigned char c : msg) {
        if (is_space(c)) {
            pending_space = !one_line.empty();
            continue;
        }
        if (pending_space) {
            one_line.push_back(' ');
            pending_space = false;
        }
        one_line.push_back(static_cast<char>(c));
    }

    if (one_line.empty()) {
        const char* st = r.status_string();
        one_line = st ? st : "query failed";
    }

    // And capped, because this text is stored, not only printed.
    //
    // The LINE excerpt PostgreSQL adds is trimmed by the server, but the CONTEXT
    // line is not: it quotes the failing statement whole, so the message grows
    // with the statement, and db-platform builds long dynamic ones. The consumers
    // above put this into db.object_text.label, which carries two btree indexes —
    // an index row over roughly 2.7 KB is rejected, api.set_object_label swallows
    // that in its EXCEPTION block and returns false, and the C++ side discards the
    // result. The label would simply not be set, and nothing would say so: the
    // very shape this card exists to remove, one floor down.
    //
    // Cut on a character boundary — messages are UTF-8 and some of this system's
    // are Russian, raised by db-platform itself rather than by PostgreSQL, so
    // lc_messages does not keep them ASCII.
    constexpr std::size_t k_max_error_text = 1024;

    if (one_line.size() > k_max_error_text) {
        std::size_t cut = k_max_error_text;
        while (cut > 0 && (static_cast<unsigned char>(one_line[cut]) & 0xC0) == 0x80)
            --cut;
        one_line.resize(cut);
        one_line += "...";
    }

    return one_line;
}

} // namespace

// ── PgResult ──────────────────────────────────────────────────────────────────

PgResult::PgResult(PGresult* res)
    : res_(res, &PQclear)
{
}

PgResult::~PgResult() = default;

PgResult::PgResult(PgResult&& o) noexcept
    : res_(std::move(o.res_))
    , withheld_(std::move(o.withheld_))
{
}

PgResult& PgResult::operator=(PgResult&& o) noexcept
{
    if (this != &o) {
        res_      = std::move(o.res_);
        withheld_ = std::move(o.withheld_);
    }
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
    if (!withheld_.empty())
        return withheld_.c_str();
    return PQresultErrorMessage(res_.get());
}

PgResult PgResult::lost(std::string text)
{
    // Without a connection PQmakeEmptyPGresult copies no error buffer: the
    // only text the result carries is the one given here.
    PgResult r(PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR));
    r.withheld_ = std::move(text);
    return r;
}

void PgResult::withhold_statement()
{
    if (!res_ || ok())
        return;

    const char* severity = PQresultErrorField(res_.get(), PG_DIAG_SEVERITY);
    const char* sqlstate = PQresultErrorField(res_.get(), PG_DIAG_SQLSTATE);
    const char* primary  = PQresultErrorField(res_.get(), PG_DIAG_MESSAGE_PRIMARY);

    // No fields: not a server error but libpq's own (a lost connection), whose
    // text never carries the statement.
    if (!primary)
        return;

    // The primary message stays, except where it quotes a value from the
    // statement: a data exception (class 22 — invalid input syntax for type
    // uuid: "<value>") and a syntax error (42601 — at or near "<token>"). There
    // the whole message goes, not only the quoted part: the quote marks follow
    // lc_messages (»…« in German, « … » in French) and a value containing a
    // quote is not escaped, so cutting between quotes is not a guarantee.
    // Elsewhere the quotes hold names — relation "db.x" does not exist — which
    // are what a reader needs and carry nothing that was sent.
    const std::string state = sqlstate ? sqlstate : "";
    const std::string_view text(primary);

    // Any quote mark of any translation: ASCII, «», „“, ‘’, ».
    static constexpr std::string_view kQuotes[] = {
        "\"", "'", "\u00AB", "\u00BB", "\u201E", "\u201C", "\u201D", "\u2018", "\u2019", "\u201A"};
    const bool quotes_something = std::any_of(std::begin(kQuotes), std::end(kQuotes),
        [text](std::string_view q) { return text.find(q) != std::string_view::npos; });

    const bool quotes_values =
        (state.rfind("22", 0) == 0 || state == "42601") && quotes_something;

    std::string reduced = quotes_values
        ? "(message withheld: it quotes a value from the statement)"
        : std::string(text);

    // From the context, the lines that locate the failure in PL/pgSQL —
    // "PL/pgSQL function kernel.x(text) line 12 at SQL statement" — and only
    // those without quotes: the other context lines ("SQL statement \"…\"")
    // are statement text. A scheduled job's label, which is where this text
    // ends up (TaskScheduler), is useless without them.
    if (const char* context = PQresultErrorField(res_.get(), PG_DIAG_CONTEXT)) {
        std::string_view rest(context);
        while (!rest.empty()) {
            const auto eol  = rest.find('\n');
            const auto line = rest.substr(0, eol);
            if (line.rfind("PL/pgSQL function ", 0) == 0 &&
                line.find_first_of("\"'") == std::string_view::npos) {
                reduced += "; ";
                reduced += line;
            }
            if (eol == std::string_view::npos)
                break;
            rest.remove_prefix(eol + 1);
        }
    }

    withheld_ = fmt::format("{}:  {} (SQLSTATE {})\n",
                            severity ? severity : "ERROR", reduced,
                            sqlstate ? sqlstate : "?????");
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

const char* PgResult::sqlstate() const
{
    const char* code = res_ ? PQresultErrorField(res_.get(), PG_DIAG_SQLSTATE) : nullptr;
    return code ? code : "";
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
    if (result_handler_ && !detached_)
        result_handler_(std::move(results));
}

void PgQuery::fail(std::string_view error)
{
    if (exception_handler_ && !detached_)
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
    start_handshake_clock();
    return true;
}

// Asked of the live PGconn, not parsed out of conninfo_: PQconninfo reports
// what libpq will use, the environment (PGCONNECT_TIMEOUT) and a service file
// included. libpq has already refused a value that is not a whole number
// (PQconnectStart fails), so only the range is ours to guard.
void PgConnection::start_handshake_clock()
{
    long timeout = 0;

    if (PQconninfoOption* opts = conn_ ? PQconninfo(conn_.get()) : nullptr) {
        for (const PQconninfoOption* o = opts; o->keyword; ++o) {
            if (o->val && *o->val && std::string_view(o->keyword) == "connect_timeout") {
                timeout = std::strtol(o->val, nullptr, 10);
                break;
            }
        }
        PQconninfoFree(opts);
    }

    handshake_late_ = false;
    if (timeout <= 0) {
        handshake_limit_    = std::chrono::seconds(0);
        handshake_deadline_ = {};
        return;
    }
    // libpq: "a value of 1 is interpreted as 2"; and it takes an int.
    timeout = std::clamp(timeout, 2L, static_cast<long>(std::numeric_limits<int>::max()));

    handshake_limit_    = std::chrono::seconds(timeout);
    handshake_deadline_ = std::chrono::steady_clock::now() + handshake_limit_;
}

bool PgConnection::handshake_expired(std::chrono::steady_clock::time_point now)
{
    if (state_ != PgConnState::Connecting || handshake_limit_.count() <= 0)
        return false;

    if (conn_ && PQstatus(conn_.get()) == CONNECTION_STARTED) {
        // TCP connect in flight: the kernel's to bound, not ours (see the header).
        handshake_deadline_ = now + handshake_limit_;
        handshake_late_     = false;
        return false;
    }

    if (now <= handshake_deadline_)
        return false;

    if (!handshake_late_) {
        handshake_late_ = true;  // judged on the next tick, if still here
        return false;
    }
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
    cancel_pending_ = 0;
    if (!PQresetStart(conn_.get())) {
        state_ = PgConnState::Error;
        return false;
    }
    PQsetnonblocking(conn_.get(), 1);  // restore nonblocking after reset
    state_ = PgConnState::Connecting;
    resetting_ = true;
    last_fd_ = PQsocket(conn_.get());
    start_handshake_clock();
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

bool PgConnection::answer_buffered() const
{
    return conn_ && state_ == PgConnState::Busy && PQisBusy(conn_.get()) == 0;
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
    // Read what the server may already have said. A server that ended this
    // session while it sat idle — a restart, pg_terminate_backend,
    // idle_session_timeout — sent FATAL and closed; PQstatus does not know
    // until something is read, and PQsendQuery into that socket succeeds. The
    // query would then be lost on a connection that never carried it, and the
    // pool, which does not repeat a query once sent (T627, PgRetry::never),
    // would fail it. Found here, the query is not sent: the caller re-queues it.
    //
    // Twice: libpq's read takes what is there and returns, so the first call
    // reads the FATAL and only the second meets the close behind it. On a
    // live idle connection both are one recv() each that finds nothing.
    if (PQconsumeInput(conn_.get()) == 0 || PQconsumeInput(conn_.get()) == 0 || !connected()) {
        state_ = PgConnState::Error;
        return false;
    }
    if (PQsendQuery(conn_.get(), sql.c_str()) == 0)
        return false;
    // libpq cleared its error buffer for this query: nothing left to withhold.
    withheld_error_.clear();
    int flush_ret = PQflush(conn_.get());
    if (flush_ret == -1)
        return false;  // flush error — treat as send failure
    needs_flush_ = (flush_ret == 1);  // 1 = more data to send, need EPOLLOUT
    state_ = PgConnState::Busy;
    heard_ = std::chrono::steady_clock::now();  // the silence counts from here
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
    if (!conn_)
        return "";

    const char* msg = PQerrorMessage(conn_.get());
    if (withheld_error_.empty())
        return msg;

    // Compared by text, not by length: if libpq has reset its buffer since,
    // the head is gone and the whole message is new.
    const std::string_view all(msg);
    if (all.rfind(withheld_error_, 0) != 0)
        return msg;

    const char* tail = msg + withheld_error_.size();
    while (*tail == '\n' || *tail == ' ')
        ++tail;
    return *tail ? tail : "connection error (the previous statement's error is withheld)";
}

void PgConnection::withhold_last_error()
{
    if (conn_)
        withheld_error_ = PQerrorMessage(conn_.get());
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
    set_reset({"current.area"});
}

namespace {

/// "all", any case — RESET ALL.
bool is_reset_all(std::string_view name)
{
    if (name.size() != 3)
        return false;
    for (std::size_t i = 0; i < 3; ++i)
        if ((name[i] | 0x20) != "all"[i])
            return false;
    return true;
}

} // namespace

bool PgPool::valid_reset_name(std::string_view name)
{
    if (is_reset_all(name))
        return true;
    // A custom parameter: two or more dot-separated identifiers. A plain name is
    // a server parameter, and RESET of one the role may not set, or the server
    // does not know, fails — and with it every query of the pool.
    std::size_t parts = 0;
    while (true) {
        const auto dot  = name.find('.');
        const auto part = name.substr(0, dot);
        if (part.empty() || part.size() > 63)   // NAMEDATALEN - 1: longer is cut
            return false;
        const char f = part.front();
        if (!((f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z') || f == '_'))
            return false;
        for (char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        ++parts;
        if (dot == std::string_view::npos)
            break;
        name.remove_prefix(dot + 1);
    }
    return parts >= 2;
}

void PgPool::set_reset(const std::vector<std::string>& names)
{
    // Spliced into SQL: only a name that cannot carry anything else, each part
    // quoted so that a keyword ("user", "all") is a name. Parameter names are
    // matched without regard to case, quoted or not.
    std::string sql;
    for (const auto& n : names) {
        if (!valid_reset_name(n))
            throw std::invalid_argument(fmt::format("not a custom configuration parameter name: '{}'", n));
        if (is_reset_all(n)) {
            sql += "RESET ALL; ";
            continue;
        }
        std::string quoted;
        std::string_view rest(n);
        while (true) {
            const auto dot = rest.find('.');
            quoted += fmt::format("\"{}\"", rest.substr(0, dot));
            if (dot == std::string_view::npos)
                break;
            quoted += '.';
            rest.remove_prefix(dot + 1);
        }
        sql += fmt::format("RESET {}; ", quoted);
    }
    reset_names_ = names;
    reset_sql_   = std::move(sql);
    reset_count_ = names.size();
}

bool PgPool::send_query(PgConnection& conn, const PgQuery& q)
{
    // An empty statement is answered "empty query", which the caller has always
    // got as a failure; behind the reset it would be a success with no rows.
    const bool blank = q.sql().find_first_not_of(" \t\r\n;") == std::string::npos;
    const bool reset = !reset_sql_.empty() && !blank;
    const bool sent  = reset ? conn.send_query(reset_sql_ + q.sql())
                             : conn.send_query(q.sql());
    conn.set_reset_results(sent && reset ? reset_count_ : 0);
    if (sent)
        arm_stall_watch();
    return sent;
}

void PgPool::strip_reset_results(PgConnection& conn, std::vector<PgResult>& results)
{
    std::size_t n = std::min(conn.reset_results(), results.size());
    conn.set_reset_results(0);
    std::size_t ok = 0;
    while (ok < n && results[ok].ok())
        ++ok;
    results.erase(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(ok));
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
    if (handshake_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(handshake_timer_);
        handshake_timer_ = EventLoop::kInvalidTimer;
    }

    if (lost_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(lost_timer_);
        lost_timer_ = EventLoop::kInvalidTimer;
    }
    if (stall_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(stall_timer_);
        stall_timer_ = EventLoop::kInvalidTimer;
    }
    cancel_listener_answer();

#ifdef LIBPQ_HAS_ASYNC_CANCEL
    // A cancel still on its way goes with the pool: its query's result has
    // nobody to reach any more.
    for (auto& cr : cancels_) {
        if (cr->fd >= 0)
            loop_.remove_io(cr->fd);
        if (cr->timer != EventLoop::kInvalidTimer)
            loop_.cancel_timer(cr->timer);
    }
    cancels_.clear();
#endif

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
    conn->set_round(connect_round_);
    setup_notice_handler(*conn);

    if (!conn->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: connect_start failed");
        record_connect_failure(conn->round());
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: CONNECTION_STARTED", conn_tag(*conn));

    PgConnection* raw = conn.get();
    conns_.push_back(std::move(conn));
    registered_conns_.insert(raw);

    // A connection epoll refused is not kept (T767): left in Connecting with no
    // events, nothing would ever finish or replace it, and healthy_count()
    // would count it — ensure_min_connections() would not open another.
    try {
        loop_.add_io(raw->fd(), EPOLLIN | EPOLLOUT, [this, raw](uint32_t events) {
            on_io(*raw, events);
        });
    } catch (...) {
        registered_conns_.erase(raw);
        conns_.pop_back();
        throw;
    }
    if (raw->handshake_limit().count() > 0)
        arm_handshake_watch();
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
            // libpq may close the socket and open another inside one poll —
            // the next host or address (host=pg1,pg2; localhost as ::1 then
            // 127.0.0.1), target_session_attrs — and the new one usually gets
            // the same number. Comparing numbers saw no change while epoll had
            // dropped the registration with the close, so modify_io() below
            // failed with ENOENT and threw out of the loop (T596). Take the fd
            // out before polling and register whatever is there after — as
            // PgLeaderLock::on_io does.
            const int polled_fd = conn.fd();
            if (polled_fd >= 0)
                loop_.remove_io(polled_fd);

            // Use reset_poll for reconnecting connections, connect_poll for new ones
            auto ps = conn.resetting() ? conn.reset_poll() : conn.connect_poll();

            if (pg_logger_)
                pg_logger_->debug("{} {} PollingStatus: {}",
                                 conn_tag(conn),
                                 conn.resetting() ? "Reset" : "Connect",
                                 polling_status_name(ps));

            if (ps != PGRES_POLLING_FAILED && conn.fd() < 0) {
                // No socket to wait on: no event would ever come for it.
                if (pg_logger_)
                    pg_logger_->error("{} {} left without a socket (was fd={}): {}",
                                     conn_tag(conn), conn.resetting() ? "Reset" : "Connect",
                                     polled_fd, conn.error_message());
                record_connect_failure(conn.round());
                replace_connection(conn);
                break;
            }

            if (ps == PGRES_POLLING_OK) {
                if (pg_logger_)
                    pg_logger_->notice("{} {}.",
                                      conn_tag(conn),
                                      conn.resetting() ? "Reconnected" : "Connected");
                record_connect_success();
                if (watch_or_replace(conn, EPOLLIN))
                    dispatch_queue(conn);
            } else if (ps == PGRES_POLLING_FAILED) {
                if (pg_logger_)
                    pg_logger_->error("{} Connect/Reset failed: {}",
                                     conn_tag(conn), conn.error_message());
                record_connect_failure(conn.round());
                // Replace this dead connection
                replace_connection(conn);
            } else {
                // Register for what poll needs next (Issue #5)
                uint32_t want = (ps == PGRES_POLLING_READING) ? EPOLLIN
                              : (ps == PGRES_POLLING_WRITING) ? EPOLLOUT
                              : (EPOLLIN | EPOLLOUT);
                watch_or_replace(conn, want);
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
            // Input is a sign of life, whatever it turns out to be: the stall
            // watch (set_stall_timeout()) counts the silence from here.
            if (events & EPOLLIN)
                conn.mark_heard(std::chrono::steady_clock::now());

            // Handle pending flush on writable event (Issue #2)
            if ((events & EPOLLOUT) && conn.needs_flush()) {
                if (conn.flush()) {
                    // Fully flushed — only need EPOLLIN for results
                    if (!loop_.modify_io(conn.fd(), EPOLLIN)) {
                        // The loop no longer watches the socket (closed under
                        // us, T607): no result will ever be read from it.
                        // Lost like any other — the path PQconsumeInput
                        // failing takes below.
                        if (pg_logger_)
                            pg_logger_->error("{} Socket fd={} is no longer watched, reconnecting",
                                             conn_tag(conn), conn.fd());
                        fail_inflight_query(conn, "socket no longer watched");
                        if (!try_reconnect(conn))
                            replace_connection(conn);
                        break;
                    }
                }
                // If flush() returned false (PQflush==1), keep EPOLLOUT
                if (!(events & EPOLLIN))
                    break;  // no readable data yet
            }

            auto results = conn.collect_results();

            // Issue #1: PQconsumeInput failed — connection is dead
            if (conn.state() == PgConnState::Error) {
                // With a quiet query in flight, the buffer may hold its own
                // error ahead of the disconnect — both arriving in one read,
                // before withhold_last_error() had a result to mark it by.
                // Rare (the server does not drop a session over an ERROR), but
                // the cost of a generic line is nil.
                const bool quiet_inflight =
                    conn.current_query() && conn.current_query()->quiet();
                const char* reason = quiet_inflight
                    ? "connection lost (the in-flight statement is quiet; its text is withheld)"
                    : conn.error_message();
                if (pg_logger_)
                    pg_logger_->error("{} PQconsumeInput failed: {}",
                                     conn_tag(conn), reason);
                fail_inflight_query(conn, reason);
                if (!try_reconnect(conn))
                    replace_connection(conn);
                break;
            }

            // The reset sent ahead of the query (set_reset()) is not the
            // caller's: a complete batch is handed over without its results.
            if (!results.empty())
                strip_reset_results(conn, results);

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

            // A quiet statement carries credentials in its literals, and libpq
            // quotes the statement back in its error text. Reduce that text
            // before anyone sees it: the log line below, the query's exception
            // handler (which modules log, and AuthServer answers a client with)
            // and a caller reading error_message() itself (T306).
            const bool quiet = conn.current_query() && conn.current_query()->quiet();
            if (quiet) {
                for (auto& r : results)
                    r.withhold_statement();
                // libpq also keeps that error in the connection's own buffer,
                // and a connection lost while idle is reported with it at the
                // head (PQerrorMessage appends).
                if (std::any_of(results.begin(), results.end(),
                                [](const PgResult& r) { return !r.ok(); }))
                    conn.withhold_last_error();
            }

            if (pg_logger_) {
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
                        // A failed statement goes to the error handler — which until
                        // 08.09.2026 it never did. deliver() ran whatever came back,
                        // so every on_exception in the ecosystem was unreachable code
                        // and an SQL error arrived dressed as a result. Callers that
                        // did not test ok() — nearly all of them — then recorded the
                        // failure as a success: a scheduled job whose body threw came
                        // back 'enabled' with its next run time advanced, and nothing
                        // was written anywhere a person would look (card T224).
                        auto bad = std::find_if(results.begin(), results.end(),
                            [](const PgResult& r) { return !r.ok(); });

                        if (bad == results.end()) {
                            owned->deliver(std::move(results));
                        } else if (owned->has_exception_handler()) {
                            owned->fail(result_error_text(*bad));
                        } else {
                            // Nobody to tell, so say it here rather than nowhere —
                            // and still hand the results over. A caller without an
                            // error handler is one that reads ok() itself, that being
                            // the only way an error was ever visible; dropping its
                            // call would leave a deferred HTTP response unsent and
                            // the client waiting out its timeout.
                            // Tagged and numbered, and it repeats the text on
                            // purpose: the query id and the statement it ran are
                            // joined nowhere else in this file. The loop above
                            // prints the error without saying which query produced
                            // it, and the two lines that do print a statement
                            // ("{} Query: {}") carry no id. A log line that cannot
                            // be read on its own is read by nobody.
                            //
                            // Note the text here is folded to one line while the
                            // loop above prints libpq's raw multi-line message, so
                            // the two are the same error in two shapes. Making that
                            // loop fold as well would change every error line in
                            // this file and is a decision of its own.
                            if (pg_logger_)
                                pg_logger_->error(
                                    "{} Query {} failed and had no error handler: {}",
                                    conn_tag(conn), owned->id(), result_error_text(*bad));
                            owned->deliver(std::move(results));
                        }
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
    // modify_io inside the switch, or by add_io in Connecting — there the
    // rearm is redundant and harmless) so the next read/write transition is
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

    // Check in-flight queries — ask PostgreSQL to cancel it
    for (auto& conn : conns_) {
        if (conn->current_query() && conn->current_query()->id() == id) {
            conn->current_query()->mark_canceled();
            std::string err;
#ifdef LIBPQ_HAS_ASYNC_CANCEL
            // One request on its way is enough; a repeated cancel() after it
            // failed sends another.
            if (conn->cancel_pending() == id)
                return true;
            bool ok = start_cancel(*conn, id, err);
            if (!ok && pg_logger_)
                pg_logger_->error("{} Cancel of query {} not started: {}",
                                 conn_tag(*conn), id, err);
#else
            bool ok = conn->cancel(err);
            if (pg_logger_) {
                if (ok)
                    pg_logger_->notice("{} Canceled query {}", conn_tag(*conn), id);
                else
                    pg_logger_->error("{} PQcancel failed for query {}: {}",
                                     conn_tag(*conn), id, err);
            }
#endif
            return ok;
        }
    }

    // Lost with its connection and waiting to be told so: not told.
    for (auto& l : lost_) {
        if (l.query->id() == id) {
            l.query->mark_canceled();
            return true;
        }
    }

    // Not in-flight — mark for removal from queue
    canceled_ids_.insert(id);
    return true;
}

#ifdef LIBPQ_HAS_ASYNC_CANCEL

// The socket is connected: the next PQcancelPoll may write the request into
// it (with sslmode=disable it does, at once).
static bool cancel_socket_connected(int fd) noexcept
{
    sockaddr_storage sa{};
    socklen_t        len = sizeof(sa);
    return fd >= 0 && ::getpeername(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0;
}

// What the exception being handled says. Inside a catch only.
static std::string current_exception_text()
{
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

// PQcancel connects to the server and waits for it on the calling thread: a
// server that takes the connection and does not answer — a paused process, a
// stuck proxy — held the loop, and every connection it serves, until the
// kernel gave up (157 s measured). Here the request is a connection of
// its own on the loop, given up by a timer: libpq does not time a
// non-blocking cancel by itself.
bool PgPool::start_cancel(PgConnection& conn, uint64_t id, std::string& error)
{
    // Whatever throws on the way — allocation, the loop refusing the timer or
    // the socket — leaves nothing behind: not started.
    CancelRequest* raw = nullptr;
    try {
        auto cr = std::make_unique<CancelRequest>();
        cr->conn.reset(conn.cancel_conn());
        if (!cr->conn) {
            error = conn.fd() < 0 ? "no connection" : "PQcancelCreate failed";
            return false;
        }
        if (PQcancelStatus(cr->conn.get()) == CONNECTION_BAD || !PQcancelStart(cr->conn.get())) {
            error = PQcancelErrorMessage(cr->conn.get());
            return false;
        }
        cr->query_id = id;
        cr->tag      = conn_tag(conn);
        cr->target   = &conn;
        cr->limit    = conn.handshake_limit().count() > 0 ? conn.handshake_limit()
                                                         : std::chrono::seconds(10);
        raw = cr.get();
        cancels_.push_back(std::move(cr));

        raw->timer = loop_.add_timer(raw->limit, [this, raw] {
            raw->timer = EventLoop::kInvalidTimer;
            if (cancel_socket_connected(raw->fd))
                raw->sent = true;
            end_cancel(raw, false, {}, /*timed_out=*/true);
        }, /*repeat=*/false);

        // libpq: after PQcancelStart, poll as if PQcancelPoll had last answered
        // PGRES_POLLING_WRITING.
        const int fd = PQcancelSocket(raw->conn.get());
        if (fd < 0)
            throw std::runtime_error("no socket");
        loop_.add_io(fd, EPOLLOUT, [this, raw](uint32_t) { on_cancel_io(raw); });
        raw->fd = fd;
    } catch (const std::exception& e) {
        if (raw)
            drop_cancel(raw);
        error = e.what();
        return false;
    }

    conn.set_cancel_pending(id);
    if (pg_logger_)
        pg_logger_->notice("{} Cancel of query {} requested", raw->tag, id);
    return true;
}

void PgPool::on_cancel_io(CancelRequest* cr)
{
    // libpq may close the socket and open another inside one poll: taken off
    // before, whatever is there registered after — as on_io() does for a
    // handshake.
    if (cr->fd >= 0) {
        if (cancel_socket_connected(cr->fd))
            cr->sent = true;
        loop_.remove_io(cr->fd);
        cr->fd = -1;
    }

    const auto ps = PQcancelPoll(cr->conn.get());
    if (ps == PGRES_POLLING_READING)
        cr->sent = true;   // waiting for the server: something has gone out

    if (ps == PGRES_POLLING_OK) {
        end_cancel(cr, true, {});
        return;
    }
    if (ps == PGRES_POLLING_FAILED) {
        end_cancel(cr, false, PQcancelErrorMessage(cr->conn.get()));
        return;
    }

    const int fd = PQcancelSocket(cr->conn.get());
    if (fd < 0) {
        end_cancel(cr, false, "no socket");
        return;
    }
    try {
        loop_.add_io(fd, ps == PGRES_POLLING_READING ? EPOLLIN : EPOLLOUT,
                     [this, cr](uint32_t) { on_cancel_io(cr); });
        cr->fd = fd;
    } catch (const std::exception& e) {
        end_cancel(cr, false, e.what());
    }
}

// Called from the request's own timer, which the loop does not guard: the
// request is gone before anything here may throw, and nothing thrown leaves.
void PgPool::end_cancel(CancelRequest* cr, bool ok, std::string_view error, bool timed_out)
{
    // The error text may be libpq's, owned by the request.
    std::string why;
    try {
        why.assign(error);
    } catch (...) {}
    std::string         tag;
    tag.swap(cr->tag);
    const uint64_t      id     = cr->query_id;
    const auto          limit  = cr->limit;
    const bool          sent   = cr->sent;
    PgConnection* const target = cr->target;
    drop_cancel(cr);

    if (pg_logger_) try {
        if (ok) {
            // Delivered: the server got the request. Whether the query was
            // still running then, only its own result tells.
            pg_logger_->notice("{} Cancel of query {} delivered", tag, id);
        } else {
            // libpq's text ends with a newline, and may span several lines.
            while (!why.empty() && (why.back() == '\n' || why.back() == ' '))
                why.pop_back();
            if (timed_out)
                why = fmt::format("no answer within {} s, given up", limit.count());
            pg_logger_->error("{} Cancel of query {} failed: {}", tag, id, why);
        }
    } catch (...) {}

    // Failed after its packet may have left — given up on a server that took
    // the connection and went silent, or the connection failed on the way — the
    // request can still reach the backend, whenever the server or a proxy in
    // between gets to it, and cancel whatever runs there then. The backend is
    // given up instead: a reset is a new one, which the request does not name.
    // Its query, if still running, is the canceled one: dropped unreported.
    // Failed before (refused, no connection), nothing went out: the
    // connection takes queries again.
    if (target && target->cancel_pending() == id) {
        try {
            if (!ok && sent) {
                if (pg_logger_) try {
                    pg_logger_->notice("{} Cancel of query {} may still arrive — reconnecting", tag, id);
                } catch (...) {}
                fail_inflight_query(*target, "reconnected: a cancel request may still arrive");
                if (!try_reconnect(*target))
                    replace_connection(*target);
            } else {
                target->set_cancel_pending(0);
                dispatch_queue(*target);
            }
        } catch (...) {
            // Never left marked for good: it would take no query again. Its
            // packet may have left — then not freed but given up as broken
            // (heartbeat() replaces it): the old backend must not take the
            // next query. Looked up, as the throw may have come after the
            // connection was replaced.
            for (auto& c : conns_) {
                if (c.get() != target || c->cancel_pending() != id)
                    continue;
                if (!ok && sent)
                    c->set_state(PgConnState::Error);
                else
                    c->set_cancel_pending(0);
            }
            if (pg_logger_) try {
                pg_logger_->error("{} After the cancel of query {}: {}", tag, id, current_exception_text());
            } catch (...) {}
        }
    }
}

void PgPool::drop_cancel(CancelRequest* cr) noexcept
{
    if (cr->fd >= 0)
        loop_.remove_io(cr->fd);
    if (cr->timer != EventLoop::kInvalidTimer)
        loop_.cancel_timer(cr->timer);

    std::erase_if(cancels_, [cr](const std::unique_ptr<CancelRequest>& p) { return p.get() == cr; });
}

#endif // LIBPQ_HAS_ASYNC_CANCEL

void PgPool::dispatch_queue(PgConnection& conn)
{
    // Skip canceled queued queries
    while (!queue_.empty()) {
        auto& front = queue_.front();
        if (canceled_ids_.count(front->id())) {
            if (pg_logger_)
                pg_logger_->debug("Dropping canceled queued query {}", front->id());
            canceled_ids_.erase(front->id());
            queue_.pop_front();
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

    // A cancel request still on its way would hit the next query:
    // end_cancel() dispatches here when it is done.
    if (conn.cancel_pending())
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

    if (pg_logger_ && !queue_.front()->quiet())
        pg_logger_->debug("{} Query: {}", conn_tag(conn), queue_.front()->sql());

    // Owned by inflight_ before the connection points at it — see execute() (T650).
    PgQuery* raw_q = queue_.front().get();
    inflight_.push_back(std::move(queue_.front()));
    queue_.pop_front();
    conn.set_current_query(raw_q);

    // If flush was incomplete, need EPOLLOUT to continue flushing. A socket
    // the loop no longer watches (T607) would leave the rest unsent for good;
    // the server has not got the whole statement, so it has not run it —
    // re-queued as when send_query fails.
    const bool sent = send_query(conn, *raw_q);
    if (sent && (!conn.needs_flush() || loop_.modify_io(conn.fd(), EPOLLIN | EPOLLOUT)))
        return;

    // Taken back and the connection marked before the log line: a throw from
    // the log leaves no query that nothing points at (T650). Re-queue the
    // query instead of losing it (P1 Fix: retry).
    const int state = static_cast<int>(conn.state());
    conn.set_current_query(nullptr);
    auto query = std::move(inflight_.back());
    inflight_.pop_back();
    enqueue(std::move(query));
    conn.set_state(PgConnState::Error);
    if (pg_logger_) {
        if (sent)
            pg_logger_->error("{} Socket fd={} is no longer watched in dispatch, re-queuing",
                             conn_tag(conn), conn.fd());
        else
            pg_logger_->error("{} send_query failed in dispatch (state={}): {}, re-queuing",
                             conn_tag(conn), state, conn.error_message());
    }
    try_reconnect(conn);
}

PgPool::QueryId PgPool::execute(std::string              sql,
                                PgQuery::ResultHandler    on_result,
                                PgQuery::ExceptionHandler on_exception,
                                bool                      quiet,
                                PgRetry                   retry)
{
    auto q = std::make_unique<PgQuery>(std::move(sql), quiet);
    q->on_result(std::move(on_result));
    q->on_exception(std::move(on_exception));
    q->set_retry(retry);

    auto qid = q->id();

    // Find a ready connection (mirrors v1 GetReadyConnection)
    for (auto& c : conns_) {
        if (c->state() == PgConnState::Ready && !c->cancel_pending()) {
            // P0 Fix: verify actual PQstatus before using (v1 checks Connected())
            if (!c->connected()) {
                if (pg_logger_)
                    pg_logger_->error("{} CONNECTION_BAD detected in execute(), reconnecting",
                                     conn_tag(*c));
                c->set_state(PgConnState::Error);
                try_reconnect(*c);
                continue;  // skip to next connection
            }

            if (pg_logger_ && !q->quiet())
                pg_logger_->debug("{} Query: {}", conn_tag(*c), q->sql());

            // inflight_ owns the query before the connection points at it
            // (T650): a throw in between leaves the pointer on a live query,
            // never on one the unwinding of this frame freed.
            PgQuery* raw_q = q.get();
            inflight_.push_back(std::move(q));
            c->set_current_query(raw_q);

            // A partial flush on a socket the loop no longer watches (T607):
            // the rest would never go out, and the server, not having the
            // whole statement, has not run it — on to the next connection, as
            // when send_query fails (connection probably died).
            const bool sent = send_query(*c, *raw_q);
            // modify_io may also throw — epoll out of memory (T767). The query
            // is taken back below as on a lost socket and the exception passed
            // on: the caller learns nothing went out, and may send it again
            // without the pool flushing the tail of this copy behind it.
            bool armed = false;
            std::exception_ptr fault;
            if (sent && c->needs_flush()) {
                try {
                    armed = loop_.modify_io(c->fd(), EPOLLIN | EPOLLOUT);
                } catch (...) {
                    fault = std::current_exception();
                }
            }
            if (sent && (!c->needs_flush() || armed))
                return qid;

            // Taken back and the connection marked before the log line: a
            // throw from the log leaves no query that nothing points at.
            const int state = static_cast<int>(c->state());
            c->set_current_query(nullptr);
            q = std::move(inflight_.back());
            inflight_.pop_back();
            c->set_state(PgConnState::Error);
            if (pg_logger_) {
                if (fault)
                    pg_logger_->error("{} Socket fd={} could not be watched for writing, reconnecting",
                                     conn_tag(*c), c->fd());
                else if (sent)
                    pg_logger_->error("{} Socket fd={} is no longer watched, reconnecting",
                                     conn_tag(*c), c->fd());
                else
                    pg_logger_->error("{} send_query failed (state={}): {}, reconnecting",
                                     conn_tag(*c), state, c->error_message());
            }
            // Part of the statement has reached the server: the reset drops the
            // session that holds it, so it is never run. If try_reconnect()
            // itself throws (its own add_io — a second, independent epoll
            // failure), that exception escapes in place of the fault: the
            // query is already taken back either way. The connection is then
            // in Error with a socket nothing watches (watch(), T789): out of
            // healthy_count(), so the next query opens another one, and
            // replaced by heartbeat(). Not here: a module calling execute()
            // from a result callback runs inside on_io() of a connection that
            // may be this one, and on_io() goes on using it.
            try_reconnect(*c);
            if (fault)
                std::rethrow_exception(fault);
            // Don't return — try next connection or queue
        }
    }

    // No ready connection — queue. Connections first: opening one may throw
    // (epoll, timerfd — T767), and a query already queued would then go out
    // with the next connection while its caller, told it failed, sends it
    // again. Nothing in here dispatches the queue, so the order is free.
    ensure_min_connections();

    enqueue(std::move(q));

    return qid;
}

void PgPool::enqueue(std::unique_ptr<PgQuery> q)
{
    // Before detach() nothing in the queue is detached, and the order is the
    // order of the calls. After it, the queue holds work nobody waits for any
    // more, and a connection that frees up would take that first — on_stop()'s
    // own statements, which do have somebody waiting, would queue behind it,
    // and with every connection held by a slow query they would not get out
    // before the pool is destroyed. Their fate is not changed otherwise: a
    // detached query still goes out if there is time.
    if (detached_ && !q->detached()) {
        auto first = std::find_if(queue_.begin(), queue_.end(),
            [](const std::unique_ptr<PgQuery>& x) { return x->detached(); });
        queue_.insert(first, std::move(q));
        return;
    }
    queue_.push_back(std::move(q));
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
    conn.set_round(connect_round_);

    if (pg_logger_)
        pg_logger_->notice("{} Reconnecting (reset_start)...", conn_tag(conn));

    // PQresetStart() closes the old socket internally, which removes it from
    // epoll.  Even when libpq re-opens a socket with the same fd number, the
    // kernel epoll registration is gone.  Always remove + re-add to be safe.
    if (old_fd >= 0)
        loop_.remove_io(old_fd);
    if (new_fd >= 0)
        watch(conn, EPOLLIN | EPOLLOUT);
    if (conn.handshake_limit().count() > 0)
        arm_handshake_watch();

    return true;
}

void PgPool::watch(PgConnection& conn, uint32_t events)
{
    try {
        loop_.add_io(conn.fd(), events, [this, &conn](uint32_t ev) {
            on_io(conn, ev);
        });
    } catch (...) {
        // No event will come for this socket. Error keeps it out of
        // healthy_count() — the next query opens another connection at once —
        // and heartbeat() replaces it (T789). Not replaced here: try_reconnect()
        // runs inside a module's call to execute() too, and that may be inside
        // on_io() of this very connection, which goes on using it.
        conn.set_state(PgConnState::Error);
        throw;
    }
}

bool PgPool::watch_or_replace(PgConnection& conn, uint32_t events)
{
    // For on_io()'s handshake only: there the connection may be replaced on
    // the spot, as its FAILED branch does — nothing above it holds the
    // connection, and the queue does not wait for the next heartbeat().
    try {
        watch(conn, events);
        return true;
    } catch (const std::exception& e) {
        if (pg_logger_)
            pg_logger_->error("{} Socket fd={} could not be watched: {}, replacing",
                             conn_tag(conn), conn.fd(), e.what());
        replace_connection(conn);
        return false;
    }
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

#ifdef LIBPQ_HAS_ASYNC_CANCEL
    // A cancel on its way to this backend outlives the connection; it has
    // nothing to dispatch on when done.
    for (auto& cr : cancels_)
        if (cr->target == &conn)
            cr->target = nullptr;
#endif

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

        // Canceled: its caller has already been told the result will be thrown
        // away (cancel(), pg.hpp: "result is silently discarded" — so even when
        // PQcancel did not get through). Re-queued, it ran a second time on the
        // new connection only for the answer to be discarded: dispatch_queue()
        // drops a canceled query by canceled_ids_, and an in-flight cancel()
        // never puts its id there — it marks the query instead (seen in T619,
        // card T627).
        if (owned->canceled()) {
            if (pg_logger_)
                pg_logger_->debug("Dropping canceled query {} after connection error: {}",
                                  owned->id(), reason);
            return;
        }

        // The statement itself is not printed in any line below. They run at
        // notice and error, and the first eighty characters of a query are quite
        // enough to expose a secret: `api.login(E'service-…', E'<secret>'` fits
        // easily. Quiet queries are quiet for a reason, and a connection error is
        // exactly when they reach these lines.

        // Part of it still in libpq's output buffer: the server never had the
        // whole message, so it cannot have run it. Safe to send again.
        if (conn.needs_flush()) {
            if (pg_logger_)
                pg_logger_->notice("Re-queuing query {} after connection error (not sent whole): {}",
                                   owned->id(), reason);
            enqueue(std::move(owned));
            return;
        }

        // Sent whole, and its caller said a repeat is harmless.
        if (owned->retry() == PgRetry::if_lost) {
            if (pg_logger_)
                pg_logger_->notice("Re-queuing query {} after connection error (retry if lost): {}",
                                   owned->id(), reason);
            enqueue(std::move(owned));
            return;
        }

        // Sent whole: the server may have run it, committed it, and lost the
        // connection only on the way back. Until T627 it was re-queued here
        // regardless — and a statement with an effect ran twice: a second row,
        // a second charge, api.*_fail twice. Whether it ran is unknowable from
        // here, so it is not repeated: its caller is told, and decides. A
        // connection the server had already closed before the query went out is
        // caught in send_query(), before sending, and there the query is
        // re-queued as it always was.
        // One line: libpq's reason spans several ("server closed the connection
        // unexpectedly\n\tThis probably means…"), and it goes into a log line
        // and, through exec_sql, into an HTTP 500 body.
        std::string one_line;
        for (char c : reason) {
            const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
            if (!space)
                one_line += c;
            else if (!one_line.empty() && one_line.back() != ' ')
                one_line += ' ';
        }
        while (!one_line.empty() && one_line.back() == ' ')
            one_line.pop_back();

        std::string error = fmt::format(
            "connection lost while the statement was in the server's hands — it may or may not "
            "have been executed, and is not repeated: {}", one_line);

        if (pg_logger_)
            pg_logger_->error("{} Query {} failed: {}", conn_tag(conn), owned->id(), error);

        // The caller hears it from the loop, not from here: this runs in the
        // middle of the pool's own handling of the loss — inside heartbeat()'s
        // walk over conns_, before try_reconnect() — and a handler that calls
        // execute() can grow conns_ under that walk.
        PgResult result = PgResult::lost(error);
        lost_.push_back(Lost{std::move(owned), std::move(error), std::move(result)});

        if (lost_timer_ == EventLoop::kInvalidTimer)
            lost_timer_ = loop_.add_timer(std::chrono::milliseconds(1),
                [this] {
                    lost_timer_ = EventLoop::kInvalidTimer;
                    deliver_lost();
                }, /*repeat=*/false);
    }
}

void PgPool::deliver_lost()
{
    // One at a time, taken out of lost_ just before its handler runs, so that a
    // cancel() or detach() from a handler still reaches the entries after it. A
    // loss is never appended synchronously from a handler (execute() re-queues a
    // query it could not send), so the loop ends.
    while (!lost_.empty()) {
        Lost l = std::move(lost_.front());
        lost_.erase(lost_.begin());

        // cancel() and detach() reach a query here as well; deliver() and fail()
        // check detached() themselves, canceled() is checked here as on_io does.
        if (l.query->canceled())
            continue;

        if (l.query->has_exception_handler()) {
            l.query->fail(l.error);
        } else {
            // As with a failed statement (see on_io): a caller without an error
            // handler still gets its call, with a result that is not ok().
            std::vector<PgResult> results;
            results.push_back(std::move(l.result));
            l.query->deliver(std::move(results));
        }
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
    pool_probed_               = false;
    pool_waited_               = false;
}

void PgPool::record_connect_failure(std::uint64_t round)
{
    using namespace std::chrono;
    // Started before the last counted failure: the outage this attempt met has
    // been counted already, and its window is running. A round is "the attempts
    // started at once" — ensure_min_connections() opening the pool, the
    // reconnect timer restoring pool and listener together — so one dead server
    // costs one step however many connections met it.
    if (round != connect_round_)
        return;
    ++connect_round_;
    // Already throttled: this failure is a fresh attempt that fired after a
    // previous backoff expired. The counter goes on — the log tells how long
    // the outage has lasted — the step stops at 4 s: 1, 2, 4, 4, … One
    // attempt per round every 4 s is nothing to a server and a line or two in
    // the log; a step doubling on to 32 s found a server back after a long
    // outage up to 32 s late — behind a bouncer that has just reopened, all
    // of that delay is the pool's. Not under twice the shutdown round
    // (Application::finish_work, 1.5 s): a step within the round keeps a
    // reconnect due on every round of a stop (fail_stranded), and the stop
    // waits for it.
    if (consecutive_connect_fails_ < std::numeric_limits<int>::max())
        consecutive_connect_fails_++;
    const int delay_sec = 1 << (std::min(consecutive_connect_fails_, 3) - 1);
    next_connect_attempt_ = steady_clock::now() + seconds(delay_sec);
}

void PgPool::reconnect_now()
{
    if (reconnect_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(reconnect_timer_);
        reconnect_timer_ = EventLoop::kInvalidTimer;
    }
    next_connect_attempt_ = {};
    if (pg_logger_)
        pg_logger_->notice("PgPool: reconnecting now, past the backoff");
    // What the reconnect timer does, and a listener that died on the outage.
    ensure_min_connections();
    if (listener_ && listener_is_dead())
        restart_listener("down while queries wait for it");
    else if (!listener_ && wants_listener())
        start_listener();
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
            if (!listener_ && wants_listener())
                start_listener();
        },
        /*repeat=*/false);
}

void PgPool::arm_handshake_watch() noexcept
{
    if (handshake_timer_ != EventLoop::kInvalidTimer)
        return;  // already armed; it re-arms itself while a handshake is in flight

    // A timer the loop refuses (timerfd, epoll — T789) is no reason to fail
    // the connection that asked for it: thrown out of new_connection() or
    // try_reconnect(), it told execute()'s caller that a query failed with a
    // live connection under it; out of on_handshake_watch(), it left the
    // loop. The handshake runs on its socket's events either way and only
    // lacks its limit until the next start or heartbeat() arms the watch.
    try {
        handshake_timer_ = loop_.add_timer(std::chrono::seconds(1),
            [this] { on_handshake_watch(); },
            /*repeat=*/false);
    } catch (const std::exception& e) {
        if (pg_logger_) {
            try {
                pg_logger_->error("PgPool: handshake watch not armed: {}", e.what());
            } catch (...) {}
        }
    } catch (...) {
        if (pg_logger_) {
            try {
                pg_logger_->error("PgPool: handshake watch not armed: unknown exception");
            } catch (...) {}
        }
    }
}

void PgPool::on_handshake_watch()
{
    handshake_timer_ = EventLoop::kInvalidTimer;
    const auto now = std::chrono::steady_clock::now();

    // A handshake past its limit is judged without reading the socket:
    // libpq wants PQconnectPoll called only once the socket is ready for what
    // it last asked, and the timer does not know that. Instead a verdict takes
    // two ticks (handshake_expired()), so that an answer that arrived in the
    // same wake-up as this timer is read by on_io() before it is judged.
    //
    // Every connection in Connecting is asked each tick — the question keeps
    // its clock. Collected first: replace_connection() erases from conns_ and
    // appends the replacement.
    std::vector<PgConnection*> expired;
    for (const auto& c : conns_)
        if (c->handshake_expired(now))
            expired.push_back(c.get());

    for (PgConnection* c : expired) {
        if (pg_logger_)
            pg_logger_->error("{} Connect/Reset failed: handshake not finished in {}s",
                             conn_tag(*c), c->handshake_limit().count());
        record_connect_failure(c->round());
        replace_connection(*c);
    }

    // As the FAILED branch of on_listener_io: nothing was shipped during the
    // handshake, so pending_listens_ still holds every channel.
    if (listener_ && listener_->handshake_expired(now)) {
        if (pg_logger_)
            pg_logger_->error("{} Listener Error: handshake not finished in {}s",
                             conn_tag(*listener_), listener_->handshake_limit().count());
        record_connect_failure(listener_->round());
        if (listener_->fd() >= 0)
            loop_.remove_io(listener_->fd());
        listener_.reset();
        schedule_reconnect_timer();
    }

    if (handshake_in_flight())
        arm_handshake_watch();
}

bool PgPool::handshake_in_flight() const
{
    auto in_flight = [](const PgConnection& c) {
        return c.state() == PgConnState::Connecting && c.handshake_limit().count() > 0;
    };
    if (listener_ && in_flight(*listener_))
        return true;
    for (const auto& c : conns_)
        if (in_flight(*c))
            return true;
    return false;
}

std::size_t PgPool::outstanding() const
{
    std::size_t n = 0;

    for (const auto& q : queue_)
        if (!q->detached())
            ++n;

    for (const auto& conn : conns_)
        if (conn && conn->current_query() && !conn->current_query()->detached())
            ++n;

    for (const auto& l : lost_)
        if (!l.query->detached() && !l.query->canceled())
            ++n;

    // A listener statement too: what on_stop() sends there (a deregistration)
    // is waited for by the drain after it, like a pool query.
    for (const auto& lq : listener_queue_)
        if (!lq.query->detached())
            ++n;

    if (listener_query_ && !listener_query_->detached())
        ++n;

    return n;
}

// A Connecting listener counts as up, as healthy_count() counts a Connecting
// connection: its handshake ends, one way or the other, within the handshake
// limit.
bool PgPool::listener_reachable() const
{
    return listener_ && listener_->fd() >= 0 && !listener_is_dead();
}

std::size_t PgPool::fail_stranded(std::string_view reason, std::chrono::milliseconds patience)
{
    // A cancelled query is done, whatever the database does: its result is
    // discarded (cancel()), and dispatch_queue() would only drop it. Dropped
    // now, so that it holds the finish no longer — outstanding() counts it.
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (canceled_ids_.count((*it)->id())) {
            canceled_ids_.erase((*it)->id());
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }

    // A reconnect throttled by the backoff (new_connection, start_listener)
    // leaves nothing connecting until reconnect_timer_ fires: due within
    // @p patience, the database may be back by then — a restart of the server
    // or of the bouncer in front of it — and the query is left to it.
    // Down counts only with a query waiting on that side: an empty queue has
    // nothing to fail, and must not spend the attempt below — nor drop the
    // backoff — for a listener nobody is waiting on. Waited for once per
    // side: the backoff's first steps (1, 2 s) can both fall due within two
    // rounds of a long stop in a row, and a side that waited already attempts
    // past the backoff below instead — a fresh outage is failed by the third
    // call at the latest (Application::finish_work: rounds of a quarter of
    // the budget).
    const auto sides = [this, patience] {
        const bool reconnect_due = reconnect_timer_ != EventLoop::kInvalidTimer
            && next_connect_attempt_ <= std::chrono::steady_clock::now() + patience;
        const bool pool_waits = std::any_of(queue_.begin(), queue_.end(),
            [](const auto& q) { return !q->detached(); });
        const bool listener_waits = std::any_of(listener_queue_.begin(), listener_queue_.end(),
            [](const auto& e) { return !e.query->detached(); });
        bool pool_down     = pool_waits && healthy_count() == 0;
        bool listener_down = listener_waits && !listener_reachable();
        if (reconnect_due) {
            const bool pool_waited     = pool_waited_;
            const bool listener_waited = listener_waited_;
            pool_waited_     = pool_waited || pool_down;
            listener_waited_ = listener_waited || listener_down;
            pool_down     = pool_down && pool_waited;
            listener_down = listener_down && listener_waited;
        }
        return std::pair{pool_down, listener_down};
    };
    auto [pool_down, listener_down] = sides();

    if (!pool_down && !listener_down)
        return 0;

    // Not due within the patience is a schedule, not an answer: the server
    // may be back since the last attempt failed, and the next one may be up
    // to the backoff's cap away. A side found stranded for the first time
    // gets one attempt now, past the backoff, and nothing of it is failed; it
    // is failed by a later call, once that attempt has failed too (a
    // handshake in flight counts as up, see healthy_count()). Per side: the
    // listener connecting says nothing of the pool's connections.
    const bool pool_probed     = pool_probed_;
    const bool listener_probed = listener_probed_;
    bool refused = false;
    if ((pool_down && !pool_probed) || (listener_down && !listener_probed)) {
        pool_probed_     = pool_probed || pool_down;
        listener_probed_ = listener_probed || listener_down;
        try {
            reconnect_now();
        } catch (const std::exception& e) {
            // Refused by the loop: what it did start is waited for (sides()
            // below), the rest has nothing to wait for.
            refused = true;
            if (pg_logger_)
                pg_logger_->error("PgPool: reconnect for the stranded queries failed: {}", e.what());
        }
        std::tie(pool_down, listener_down) = sides();
        pool_down     = pool_down && (pool_probed || refused);
        listener_down = listener_down && (listener_probed || refused);
        if (!pool_down && !listener_down)
            return 0;
    }

    // Taken out of the queues first, all of them, before anything that may
    // throw: a slot left moved-from in queue_ would be a null pointer for
    // detach() and outstanding(). And a handler run by deliver_lost() below
    // may execute() again: what it queues is not failed by this call.
    std::vector<std::unique_ptr<PgQuery>> out;
    out.reserve((pool_down ? queue_.size() : 0) + (listener_down ? listener_queue_.size() : 0));

    if (pool_down) {
        for (auto it = queue_.begin(); it != queue_.end();) {
            if ((*it)->detached()) {
                ++it;
            } else {
                out.push_back(std::move(*it));
                it = queue_.erase(it);
            }
        }
    }

    if (listener_down) {
        for (auto it = listener_queue_.begin(); it != listener_queue_.end();) {
            if (it->query->detached()) {
                ++it;
            } else {
                out.push_back(std::move(it->query));
                it = listener_queue_.erase(it);
            }
        }
    }

    const std::size_t n = out.size();
    for (auto& q : out) {
        std::string error = fmt::format("not sent: {}", reason);
        if (pg_logger_ && !q->quiet())
            pg_logger_->error("Query {} failed: {}", q->id(), error);
        PgResult result = PgResult::lost(error);
        lost_.push_back(Lost{std::move(q), std::move(error), std::move(result)});
    }

    if (n > 0)
        deliver_lost();

    return n;
}

void PgPool::mute_listeners() noexcept
{
    listeners_muted_ = true;
}

void PgPool::detach() noexcept
{
    for (auto& q : queue_)
        q->mark_detached();

    for (auto& q : inflight_)
        q->mark_detached();

    for (auto& l : lost_)
        l.query->mark_detached();

    for (auto& lq : listener_queue_)
        lq.query->mark_detached();

    if (listener_query_)
        listener_query_->mark_detached();

    detached_ = true;
    mute_listeners();
}

std::size_t PgPool::healthy_count() const
{
    // A Busy connection past the stall limit is still counted: drop_stalled()
    // gives it up within a second and reconnects it in its own slot. Counted
    // out, it made every ensure_min_connections() in that second open a second
    // connection for the same slot — kept for good once the first recovered, as
    // the pool never shrinks.
    std::size_t count = 0;
    for (const auto& c : conns_) {
        auto s = c->state();
        if ((s == PgConnState::Ready || s == PgConnState::Busy || s == PgConnState::Connecting)
            && c->fd() >= 0)
            count++;
    }
    return count;
}

bool PgPool::stalled(const PgConnection& conn, std::chrono::steady_clock::time_point now) const
{
    return stall_timeout_.count() > 0
        && conn.state() == PgConnState::Busy
        && now - conn.heard() > stall_timeout_;
}

void PgPool::arm_stall_watch() noexcept
{
    if (stall_timeout_.count() <= 0 || stall_timer_ != EventLoop::kInvalidTimer)
        return;

    // As arm_handshake_watch(): a timer the loop refuses fails no query;
    // heartbeat() runs the same check, and the next query sent arms it again.
    try {
        stall_timer_ = loop_.add_timer(std::chrono::seconds(1),
            [this] { on_stall_watch(); },
            /*repeat=*/false);
    } catch (const std::exception& e) {
        if (pg_logger_) {
            try {
                pg_logger_->error("PgPool: stall watch not armed: {}", e.what());
            } catch (...) {}
        }
    } catch (...) {
        if (pg_logger_) {
            try {
                pg_logger_->error("PgPool: stall watch not armed: unknown exception");
            } catch (...) {}
        }
    }
}

void PgPool::on_stall_watch()
{
    stall_timer_ = EventLoop::kInvalidTimer;

    // Re-armed before the check: a throw out of the drop below (a reconnect the
    // loop refuses) leaves the loop, and the watch must outlive it.
    if (std::any_of(conns_.begin(), conns_.end(),
                    [](const std::unique_ptr<PgConnection>& c) {
                        return c->state() == PgConnState::Busy;
                    }))
        arm_stall_watch();

    drop_stalled(std::chrono::steady_clock::now());
}

void PgPool::drop_stalled(std::chrono::steady_clock::time_point now)
{
    if (stall_timeout_.count() <= 0)
        return;

    // Collected first: replace_connection() erases from conns_ and appends.
    std::vector<PgConnection*> silent;
    for (const auto& c : conns_)
        if (stalled(*c, now))
            silent.push_back(c.get());

    bool dropped = false;

    for (PgConnection* c : silent) {
        // Input waiting on the socket is an answer this loop has not read yet —
        // after a stall of the loop itself, the timer and the answer arrive in
        // one wake-up. Not judged; on_io() reads it.
        if (c->fd() >= 0) {
            pollfd p{c->fd(), POLLIN, 0};
            if (::poll(&p, 1, 0) > 0) {
                c->mark_heard(now);
                continue;
            }
        }

        const auto reason = fmt::format(
            "no answer from the server in {}s (postgres.stall_timeout)", stall_timeout_.count());
        if (pg_logger_)
            pg_logger_->error("{} Query in flight: {}, dropping the connection without cancel",
                             conn_tag(*c), reason);

        // No cancel request: the session, if it is still alive over there,
        // ends when the server notices the socket gone. And before libpq 17 a
        // cancel connects to the server synchronously — an address that has
        // stopped answering held the loop for as long as the kernel tried.
        fail_inflight_query(*c, reason);
        if (!try_reconnect(*c))
            replace_connection(*c);
        dropped = true;
    }

    // A query taken back above (not sent whole, PgRetry::if_lost) is in the
    // queue, and the connection it came off is reconnecting to a peer that has
    // probably gone: hand it to an idle one now rather than at the next
    // heartbeat(). Collected first — dispatch_queue() may reset a connection.
    if (dropped && !queue_.empty()) {
        std::vector<PgConnection*> idle;
        for (const auto& c : conns_)
            if (c->state() == PgConnState::Ready)
                idle.push_back(c.get());
        for (PgConnection* c : idle) {
            if (queue_.empty())
                break;
            dispatch_queue(*c);
        }
    }
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

    // A connection still in Error here has nothing left to drive it: every
    // path that sets Error goes on at once to try_reconnect() or
    // replace_connection(), and Error remains only when the reset could not
    // start (execute(), dispatch_queue() ignore try_reconnect()'s result), when
    // the loop refused the new socket there (watch(), T789), or when a throw
    // cut that path short. No event will come for it: libpq closed its socket,
    // or the loop does not watch it. Collected first — replace_connection()
    // erases from conns_. A query still on it is failed as lost, not dropped
    // with it — none is today, every path takes it off first.
    std::vector<PgConnection*> dead;
    for (const auto& c : conns_)
        if (c->state() == PgConnState::Error)
            dead.push_back(c.get());
    for (PgConnection* c : dead) {
        if (pg_logger_)
            pg_logger_->error("{} Left in Error with nothing to drive it, replacing",
                             conn_tag(*c));
        fail_inflight_query(*c, "connection lost");
        replace_connection(*c);
    }

    // A query in flight the server has gone silent on — the stall watch's
    // check, here as well for a watch the loop refused to arm.
    drop_stalled(std::chrono::steady_clock::now());

    // Ensure min_conns
    ensure_min_connections();

    // A handshake in flight — the listener's included — whose watch the loop
    // refused (arm_handshake_watch) gets it now; already armed, this does
    // nothing.
    if (handshake_in_flight())
        arm_handshake_watch();

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
    } else if (wants_listener()) {
        // Subscriptions or listener statements outstanding and no listener object at all.
        if (pg_logger_)
            pg_logger_->error("PgPool: {} LISTEN channel(s), {} listener statement(s) with no listener — restarting",
                             notify_handlers_.size(), listener_queue_.size());
        start_listener();
    }
}

// ── PgPool — LISTEN / NOTIFY ──────────────────────────────────────────────────

void PgPool::listen(const std::string& channel, NotifyHandler cb, ListenReadyHandler on_ready)
{
    bool is_new_channel = !notify_handlers_.contains(channel);
    notify_handlers_[channel].push_back(std::move(cb));

    // A second subscriber to a live channel would otherwise wait for the next
    // reconnect to hear that it is live. Re-sending LISTEN costs nothing on
    // the server, and its answer is the one thing that may say "ready". Not
    // when an answer for the channel is already on its way or already read:
    // handlers are looked up when it is dispatched, so it covers this one.
    const bool answer_coming = sent_listens_.contains(channel) ||
                               confirmed_listens_.contains(channel);
    const bool wants_ready   = static_cast<bool>(on_ready);
    if (wants_ready)
        ready_handlers_[channel].push_back(std::move(on_ready));

    if (is_new_channel || (wants_ready && !answer_coming))
        pending_listens_.insert(channel);

    // From a notification handler, consume_notify() is iterating the listener:
    // replacing it here freed the connection under that loop. on_listener_io
    // ships the LISTEN when the batch is read.
    if (notify_depth_ > 0)
        return;

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
    ready_handlers_.erase(ch);
    pending_listens_.erase(ch);
    sent_listens_.erase(ch);
    confirmed_listens_.erase(ch);

    single_listens_.erase(ch);
    rejected_listens_.erase(ch);

    if (listener_ && listener_->state() == PgConnState::Ready)
        listener_->send_query("UNLISTEN " + pq_quote_ident(ch));
    // If listener is busy/connecting, callbacks are already removed from
    // notify_handlers_ so notifications won't be dispatched even if UNLISTEN
    // hasn't been sent yet.
}

void PgPool::listener_execute(std::string               sql,
                              PgQuery::ResultHandler    on_result,
                              PgQuery::ExceptionHandler on_exception,
                              bool                      quiet,
                              std::chrono::seconds      answer_within)
{
    auto q = std::make_unique<PgQuery>(std::move(sql), quiet);
    q->on_result(std::move(on_result));
    q->on_exception(std::move(on_exception));
    // Asked after detach(): somebody waits for it, as for execute().
    listener_queue_.push_back(ListenerQuery{std::move(q), answer_within});

    if (notify_depth_ > 0)
        return;   // from a notification handler: on_listener_io ships it when the batch is read

    if (!listener_) {
        start_listener();
    } else if (listener_is_dead()) {
        restart_listener("listener_execute() found the listener dead");
    } else if (listener_->state() == PgConnState::Ready) {
        listener_send_next();
    } else {
        // connecting or busy — sent when the listener is next idle
        watch_listener_queue();
    }
}

void PgPool::set_listener_name(std::string name)
{
    listener_name_ = std::move(name);
}

void PgPool::listener_send_next()
{
    if (!listener_ || listener_->state() != PgConnState::Ready || listener_query_)
        return;

    if (!pending_listens_.empty()) {
        send_pending_listens();
        return;
    }

    if (listener_queue_.empty())
        return;

    ListenerQuery lq = std::move(listener_queue_.front());
    listener_queue_.pop_front();

    if (pg_logger_ && !lq.query->quiet())
        pg_logger_->debug("{} Listener Query: {}", conn_tag(*listener_), lq.query->sql());

    // Held before it goes out: the answer, or the loss of the listener, is
    // what releases it.
    listener_query_ = std::move(lq.query);

    if (!listener_->send_query(listener_query_->sql())) {   // listener → Busy
        // The server has not got it: it goes first on the next listener.
        listener_queue_.push_front(ListenerQuery{std::move(listener_query_), lq.answer_within});
        restart_listener("send_query failed while shipping a listener statement");
        return;
    }

    if (lq.answer_within.count() > 0)
        arm_listener_answer(lq.answer_within);
}

void PgPool::deliver_listener_query(std::vector<PgResult> results)
{
    cancel_listener_answer();
    auto q = std::move(listener_query_);

    // A quiet statement's error text may quote the statement: reduced before
    // the log line and the handlers see it, as on_io does.
    if (q->quiet())
        for (auto& r : results)
            r.withhold_statement();

    // As on_io does for a pool query: a failed statement goes to the error
    // handler, and a caller without one gets the results and reads ok().
    auto bad = std::find_if(results.begin(), results.end(),
        [](const PgResult& r) { return !r.ok(); });

    if (bad != results.end() && pg_logger_ && listener_)
        pg_logger_->error("{} Listener Query {} failed: {}", conn_tag(*listener_), q->id(),
                          result_error_text(*bad));

    if (bad == results.end() || !q->has_exception_handler())
        q->deliver(std::move(results));
    else
        q->fail(result_error_text(*bad));
}

void PgPool::fail_listener_query(std::string_view reason)
{
    cancel_listener_answer();
    if (!listener_query_)
        return;

    std::string error = fmt::format(
        "the LISTEN connection was lost while the statement was in the server's hands — it may "
        "or may not have been executed, and is not repeated: {}", reason);

    if (pg_logger_)
        pg_logger_->error("Listener Query {} failed: {}", listener_query_->id(), error);

    // From the loop, as for a pool query (deliver_lost): this runs inside the
    // pool's own handling of the loss.
    PgResult result = PgResult::lost(error);
    lost_.push_back(Lost{std::move(listener_query_), std::move(error), std::move(result)});

    if (lost_timer_ == EventLoop::kInvalidTimer)
        lost_timer_ = loop_.add_timer(std::chrono::milliseconds(1),
            [this] {
                lost_timer_ = EventLoop::kInvalidTimer;
                deliver_lost();
            }, /*repeat=*/false);
}

void PgPool::arm_listener_answer(std::chrono::seconds limit)
{
    cancel_listener_answer();
    const auto id = listener_query_ ? listener_query_->id() : 0;
    try {
        listener_answer_timer_ = loop_.add_timer(limit,
            [this, id, limit] {
                listener_answer_timer_ = EventLoop::kInvalidTimer;
                if (listener_query_ && listener_query_->id() == id)
                    restart_listener(fmt::format("no answer to a listener statement in {}s", limit.count()));
            }, /*repeat=*/false);
    } catch (const std::exception& e) {
        // Refused by the loop: the statement goes on without its limit.
        if (pg_logger_)
            pg_logger_->error("PgPool: listener answer watch not armed: {}", e.what());
    }
}

void PgPool::watch_listener_queue()
{
    if (listener_answer_timer_ != EventLoop::kInvalidTimer)
        return;   // the statement in flight has its own limit
    if (!listener_ || listener_->state() != PgConnState::Busy)
        return;   // idle sends at once; a handshake has its own watch

    std::chrono::seconds limit{0};
    for (const auto& lq : listener_queue_)
        if (lq.answer_within.count() > 0 && (limit.count() == 0 || lq.answer_within < limit))
            limit = lq.answer_within;
    if (limit.count() == 0)
        return;

    const auto gen     = listener_gen_;
    const auto answers = listener_answers_;
    try {
        listener_answer_timer_ = loop_.add_timer(limit,
            [this, gen, answers, limit] {
                listener_answer_timer_ = EventLoop::kInvalidTimer;
                if (listener_ && listener_gen_ == gen && listener_answers_ == answers &&
                    listener_->state() == PgConnState::Busy)
                    restart_listener(fmt::format("no answer on the listener in {}s, a statement waiting", limit.count()));
                else
                    watch_listener_queue();   // it answered: watch what it is busy with now
            }, /*repeat=*/false);
    } catch (const std::exception& e) {
        if (pg_logger_)
            pg_logger_->error("PgPool: listener queue watch not armed: {}", e.what());
    }
}

void PgPool::cancel_listener_answer()
{
    if (listener_answer_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(listener_answer_timer_);
        listener_answer_timer_ = EventLoop::kInvalidTimer;
    }
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
    listener_->set_round(connect_round_);
    ++listener_gen_;
    setup_notice_handler(*listener_);

    if (!listener_->connect_start()) {
        if (pg_logger_)
            pg_logger_->error("[0] [-1] [] ConnectException: listener connect_start failed");
        record_connect_failure(listener_->round());
        listener_.reset();
        // Without this, a listener that fails synchronously at startup (e.g.
        // pgbouncer/postgres not yet accepting) is abandoned forever: nothing
        // else ever calls start_listener() again for it, since the one-shot
        // init_listen() caller only retries on ITS OWN query failing, not on
        // the listener connection specifically.
        schedule_reconnect_timer();
        return;
    }

    if (pg_logger_)
        pg_logger_->debug("{} Status: LISTENER CONNECTION_STARTED", conn_tag(*listener_));

    int fd = listener_->fd();
    loop_.add_io(fd, EPOLLIN | EPOLLOUT, [this](uint32_t events) {
        on_listener_io(events);
    });
    if (listener_->handshake_limit().count() > 0)
        arm_handshake_watch();
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

    // The statement in flight on it is lost with it; the ones waiting go out on
    // the next listener.
    fail_listener_query(why);

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

    // A LISTEN shipped on the dead connection and never answered confirms
    // nothing; the channel is in pending_listens_ again, and the next
    // listener's answer is the one that will say it is live.
    sent_listens_.clear();
    // Answered by the connection that just died: live no more. Reached from
    // inside dispatch_listen_ready() too, when a handler kills the listener.
    confirmed_listens_.clear();
    // A new connection gets the whole registry as one batch again; a channel
    // refused by the old one is tried afresh (and split out again if bad).
    single_listens_.clear();
    rejected_listens_.clear();
    sent_batch_size_ = 0;

    // Safe to call unconditionally (no hot-loop risk): start_listener() itself
    // checks next_connect_attempt_ and defers via schedule_reconnect_timer()
    // when a connect attempt would be premature or fails again.
    if (wants_listener())
        start_listener();
}

void PgPool::send_pending_listens()
{
    if (pending_listens_.empty() || !listener_)
        return;

    std::unordered_set<std::string> sent;
    if (!single_listens_.empty()) {
        auto node = single_listens_.extract(single_listens_.begin());
        pending_listens_.erase(node.value());
        sent.insert(std::move(node.value()));
    } else {
        sent = std::move(pending_listens_);
        pending_listens_.clear();
    }

    // Quoted as an identifier: a channel name is data — WebSocketAPI reads
    // them from the database — and a bare `"` in one made the whole batch a
    // syntax error, taking every other channel in it down too.
    std::string sql;
    std::string names;
    for (const auto& ch : sent) {
        sql += "LISTEN " + pq_quote_ident(ch) + ";";
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

    if (!listener_->send_query(sql)) {  // listener → Busy
        // The channels were taken out above. Losing them here would leave a
        // healthy listener subscribed to nothing — the same silent outcome
        // by a different route.
        pending_listens_.insert(sent.begin(), sent.end());
        restart_listener("send_query failed while shipping LISTEN");
        return;
    }

    sent_batch_size_ = sent.size();
    sent_listens_.insert(sent.begin(), sent.end());
}

void PgPool::on_listener_io(uint32_t events)
{
    if (!listener_) return;

    // EPOLLERR/EPOLLHUP on the listener connection = libpq socket is gone.
    // Transition to Error state so the Error branch below reconnects from
    // scratch; without this the listener would silently loop rearming a
    // dead fd under LT, or stay disarmed under ET.
    //
    // Not during the handshake: there the error is libpq's to judge. A refused
    // connect reports EPOLLERR|EPOLLHUP, and connect_poll() is what moves on to
    // the next address or host — taking it for a death restarted the listener
    // from the first host every time, and with that host down it never
    // subscribed (T596; the same as PgLeaderLock::on_io).
    if ((events & (EPOLLERR | EPOLLHUP)) && listener_->state() != PgConnState::Connecting)
        listener_->set_state(PgConnState::Error);

    switch (listener_->state()) {

        case PgConnState::Connecting: {
            // The socket may be replaced inside the poll under the same number
            // (see on_io, T596): out before polling, registered after.
            const int polled_fd = listener_->fd();
            if (polled_fd >= 0)
                loop_.remove_io(polled_fd);

            auto ps = listener_->connect_poll();

            if (pg_logger_)
                pg_logger_->debug("{} Listener PollingStatus: {}", conn_tag(*listener_), polling_status_name(ps));

            if (ps != PGRES_POLLING_FAILED && listener_->fd() < 0) {
                if (pg_logger_)
                    pg_logger_->error("{} Listener left without a socket (was fd={}): {}",
                                     conn_tag(*listener_), polled_fd, listener_->error_message());
                record_connect_failure(listener_->round());
                listener_.reset();
                schedule_reconnect_timer();
                return;
            }

            if (ps == PGRES_POLLING_OK) {
                if (pg_logger_)
                    pg_logger_->notice("{} Listener Connected.", conn_tag(*listener_));
                listener_probed_ = false;
                listener_waited_ = false;
                loop_.add_io(listener_->fd(), EPOLLIN, [this](uint32_t ev) {
                    on_listener_io(ev);
                });
                // Every new listener names itself ahead of the statements
                // queued for it — behind its LISTENs, which listener_send_next()
                // ships before anything queued; a statement, because the
                // connection string may be a URI and cannot simply be
                // appended to.
                if (!listener_name_.empty()) {
                    auto q = std::make_unique<PgQuery>(
                        "SET application_name = " + pq_quote_literal(listener_name_), true);
                    listener_queue_.push_front(ListenerQuery{std::move(q), {}});
                }
                // ships below, at the end of this handler (listener_send_next)
            } else if (ps == PGRES_POLLING_FAILED) {
                if (pg_logger_)
                    pg_logger_->error("{} Listener Error: {}", conn_tag(*listener_), listener_->error_message());
                record_connect_failure(listener_->round());
                loop_.remove_io(listener_->fd());
                listener_.reset();
                // pending_listens_ already holds every channel (it's only
                // cleared once send_pending_listens() actually ships a LISTEN,
                // which never happened here) — just get a retry scheduled.
                schedule_reconnect_timer();
            } else {
                // Register for what poll needs next (same as worker connections)
                uint32_t want = (ps == PGRES_POLLING_READING) ? EPOLLIN
                              : (ps == PGRES_POLLING_WRITING) ? EPOLLOUT
                              : (EPOLLIN | EPOLLOUT);
                loop_.add_io(listener_->fd(), want, [this](uint32_t ev) {
                    on_listener_io(ev);
                });
            }
            break;
        }

        case PgConnState::Busy: {
            // Waiting for LISTEN/UNLISTEN command response — or for the answer
            // to a listener_execute() statement
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
            ++listener_answers_;

            if (listener_query_) {
                // A handler may call anything that replaces the listener; the
                // rest of this handler is for the one it saw.
                const auto gen = listener_gen_;
                deliver_listener_query(std::move(results));
                if (!listener_ || listener_gen_ != gen)
                    return;
                if (listener_->consume_notify([this](const char* ch, const char* payload) {
                        dispatch_notify(ch, payload);
                    }) < 0)
                {
                    restart_listener("PQconsumeInput failed right after a listener statement");
                    return;
                }
                if (!listener_ || listener_gen_ != gen)
                    return;
                break;
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

            // The answer to the batch shipped last. Taken before anything below
            // can ship the next one. A batch runs as one implicit transaction,
            // so a single failed LISTEN rolls back the rest: none is confirmed.
            if (all_ok) {
                for (const auto& ch : sent_listens_)
                    rejected_listens_.erase(ch);
                confirmed_listens_.insert(sent_listens_.begin(), sent_listens_.end());
            } else if (sent_batch_size_ > 1) {
                // Which one is bad is unknown; the rest are innocent. Before,
                // the whole batch was dropped here with one error line.
                if (pg_logger_)
                    pg_logger_->error("{} LISTEN batch of {} channel(s) rejected — resending one per query",
                                      conn_tag(*listener_), sent_batch_size_);
                for (const auto& ch : sent_listens_) {
                    pending_listens_.insert(ch);
                    single_listens_.insert(ch);
                }
            } else if (!sent_listens_.empty()) {
                const auto& ch = *sent_listens_.begin();
                rejected_listens_.insert(ch);
                if (pg_logger_)
                    pg_logger_->error("{} LISTEN {} rejected by the server — channel NOT subscribed until the listener is replaced",
                                      conn_tag(*listener_), pq_quote_ident(ch));
            }
            sent_listens_.clear();
            sent_batch_size_ = 0;

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
            } else if ((all_ok || !rejected_listens_.empty()) && pg_logger_) {
                // The confirmation half of the pair: the command was shipped
                // AND the server acknowledged it. This line is what tells an
                // operator that a silent channel is subscribed rather than
                // orphaned. Worded for both commands that land here — after an
                // UNLISTEN "established" would name the wrong event, while the
                // count stays right either way. A refused channel is counted
                // out and named: the registry holds it, the server does not.
                const std::size_t active = notify_handlers_.size() - rejected_listens_.size();
                if (rejected_listens_.empty()) {
                    pg_logger_->notice("{} LISTEN active on {} channel(s)",
                                      conn_tag(*listener_), active);
                } else {
                    std::string refused;
                    for (const auto& ch : rejected_listens_) {
                        if (!refused.empty())
                            refused += ", ";
                        refused += pq_quote_ident(ch);
                    }
                    pg_logger_->error("{} LISTEN active on {} channel(s), NOT subscribed: {}",
                                      conn_tag(*listener_), active, refused);
                }
            }
            // In either case, check for notifications that arrived simultaneously
            if (listener_->consume_notify([this](const char* ch, const char* payload) {
                    dispatch_notify(ch, payload);
                }) < 0)
            {
                restart_listener("PQconsumeInput failed right after LISTEN");
                return;
            }

            // Last, and on its own guard: a handler may call listen(),
            // unlisten() or anything that replaces the listener, and the rearm
            // below must not touch a connection it did not see.
            if (!confirmed_listens_.empty()) {
                const auto gen = listener_gen_;
                dispatch_listen_ready();
                if (!listener_ || listener_gen_ != gen)
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
            // Pending listens accumulated while we were processing go out
            // below (listener_send_next) — if a handler did not send a
            // statement of its own already: the listener may be Busy now.
            break;
        }

        case PgConnState::Error:
            restart_listener(listener_->error_message());
            // start_listener() registered the replacement fd via add_io,
            // which arms it; the old fd is gone. Skip the rearm below rather
            // than touch a connection this handler just replaced.
            return;
    }

    // Whatever left the listener idle — a handshake done, an answer read, a
    // notification — what waits for it goes out now.
    if (listener_ && listener_->state() == PgConnState::Ready) {
        const auto gen = listener_gen_;
        listener_send_next();
        if (!listener_ || listener_gen_ != gen)
            return;
    }

    // A statement shipped from inside this handler — a LISTEN, an UNLISTEN, a
    // listener statement a callback sent — may have been answered already, and
    // the consume_notify() above read that answer into libpq's buffer: the
    // socket is empty, and no event will come for it. Left there, the listener
    // stayed Busy for good — every statement after it waited, the heartbeat
    // included. Read it now, as the event would have.
    if (listener_ && listener_->answer_buffered()) {
        on_listener_io(EPOLLIN);
        return;   // the call above rearms, or replaced the listener
    }

    watch_listener_queue();

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

    if (listeners_muted_)
        return;

    auto it = notify_handlers_.find(channel);
    if (it == notify_handlers_.end()) return;

    // Copy the handler list — callbacks may mutate notify_handlers_ via unlisten()
    auto handlers = it->second;
    struct Depth { int& d; explicit Depth(int& x) : d(x) { ++d; } ~Depth() { --d; } } depth(notify_depth_);
    for (const auto& cb : handlers)
        cb(channel, payload ? payload : "");
}

void PgPool::dispatch_listen_ready()
{
    if (listeners_muted_) {
        confirmed_listens_.clear();
        return;
    }

    // One channel at a time, taken out of the member set before its handlers
    // run: a handler may unlisten() a channel still waiting here (struck out,
    // so a later listen() of it waits for its own answer) or kill the
    // listener (restart_listener() empties the set — nothing it held is live).
    while (!confirmed_listens_.empty()) {
        const std::string ch =
            std::move(confirmed_listens_.extract(confirmed_listens_.begin()).value());

        auto it = ready_handlers_.find(ch);
        if (it == ready_handlers_.end())
            continue;

        // Copy the handler list — callbacks may mutate ready_handlers_
        auto handlers = it->second;
        for (const auto& cb : handlers)
            cb(ch);
    }
}

} // namespace apostol
