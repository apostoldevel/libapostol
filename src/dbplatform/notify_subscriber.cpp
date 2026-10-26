#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

// The subscriber of a db-platform `notify` channel: the statements of
// notify.subscribe, heartbeat, take, ack, nack, sweep, drain, unsubscribe and
// channel_policy, and the order they go in. See notify_subscriber.hpp.

#include "apostol/notify_subscriber.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <unistd.h>

namespace apostol::db_platform
{

namespace
{

constexpr std::chrono::seconds kDefaultHeartbeat{5};
constexpr std::chrono::seconds kDefaultSweep{2};
constexpr std::chrono::seconds kDefaultAlive{15};

/// A whole number of the text form, or nullopt.
std::optional<std::int64_t> parse_id(std::string_view text)
{
    // Anyone who may NOTIFY may write here: past the range is not a number.
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || v < 0)
        return std::nullopt;
    return v;
}

/// A module's callback, inside a loop handler: what it throws stays here.
template <typename F>
void guarded(Logger& log, std::string_view channel, std::string_view what, F&& f)
{
    try {
        f();
    } catch (const std::exception& e) {
        log.error("notify [{}]: {} threw: {}", channel, what, e.what());
    }
}

std::string text_of(const PgResult& r, int col)
{
    return r.is_null(0, col) ? std::string{} : std::string(r.value(0, col));
}

std::chrono::seconds seconds_of(const PgResult& r, int col, std::chrono::seconds fallback)
{
    if (r.is_null(0, col))
        return fallback;
    const long v = std::strtol(r.value(0, col), nullptr, 10);
    return v > 0 ? std::chrono::seconds(v) : fallback;
}

} // namespace

NotifySubscriber::NotifySubscriber(PgPool& pool, EventLoop& loop, Logger& log, Options options,
                                   RowHandler on_row, LegacyHandler on_legacy,
                                   NumberHandler on_number)
    : pool_(pool)
    , loop_(loop)
    , log_(log)
    , options_(std::move(options))
    , on_row_(std::move(on_row))
    , on_legacy_(std::move(on_legacy))
    , on_number_(std::move(on_number))
    , self_(std::make_shared<NotifySubscriber*>(this))
    , spread_(static_cast<std::minstd_rand::result_type>(::getpid()))
{
}

NotifySubscriber::~NotifySubscriber()
{
    self_.reset();
    cancel_timers();
}

// ─── Statements ──────────────────────────────────────────────────────────────

std::string NotifySubscriber::ack_sql(const NotifyRow& row, std::string_view result_sql)
{
    // One simple-query batch is one implicit transaction: an ack refused under a
    // stale token (NT409) rolls back the result before it.
    std::string sql(result_sql);
    while (!sql.empty() && (sql.back() == ';' || sql.back() == ' ' || sql.back() == '\n' ||
                            sql.back() == '\t' || sql.back() == '\r'))
        sql.pop_back();
    // On a line of its own: a "-- comment" closing result_sql would swallow the ack.
    if (!sql.empty())
        sql += "\n;\n";
    sql += fmt::format("SELECT notify.ack({}, {})", row.id, row.token);
    return sql;
}

std::string NotifySubscriber::personal_channel() const
{
    return fmt::format("{}:{}", options_.channel, number_);
}

// ─── Start ───────────────────────────────────────────────────────────────────

void NotifySubscriber::start()
{
    if (started_)
        return;
    started_ = true;

    std::string name;
    for (const auto* part : {&options_.application, &options_.role, &options_.node}) {
        if (part->empty())
            continue;
        if (!name.empty())
            name += ' ';
        name += *part;
    }
    if (!name.empty())
        pool_.set_listener_name(std::move(name));

    if (on_legacy_) {
        std::weak_ptr<NotifySubscriber*> w = self_;
        pool_.listen(options_.channel, [w](std::string_view, std::string_view payload) {
            if (auto p = w.lock(); p && !(*p)->stopped_)
                guarded((*p)->log_, (*p)->options_.channel, "legacy handler",
                        [&] { (*p)->on_legacy_(payload); });
        });
    }

    read_policy();
    arm_policy();
    subscribe();
}

void NotifySubscriber::subscribe()
{
    if (subscribing_ || draining_ || stopped_)
        return;
    subscribing_ = true;

    // On the listener: notify.subscribe records pg_backend_pid() of the
    // connection it runs on, and that has to be the listener's.
    const std::string sql = fmt::format("SELECT notify.subscribe({}, {}, {}, {})",
        pq_quote_literal(options_.node), pq_quote_literal(options_.channel),
        options_.application.empty() ? std::string("null") : pq_quote_literal(options_.application),
        options_.role.empty() ? std::string("null") : pq_quote_literal(options_.role));

    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.listener_execute(sql,
        [w](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            self.subscribing_ = false;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (!r || !r->ok() || r->rows() != 1 || r->is_null(0, 0)) {
                self.log_.error("notify [{}]: subscribe failed: {}", self.options_.channel,
                                r ? r->error_message() : "no result");
                self.arm_retry();
                return;
            }
            self.on_subscribed(static_cast<int>(std::strtol(r->value(0, 0), nullptr, 10)));
        },
        [w](std::string_view error) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            self.subscribing_ = false;
            self.log_.error("notify [{}]: subscribe failed: {}", self.options_.channel, error);
            self.arm_retry();
        },
        /*quiet=*/false, answer_within());
}

void NotifySubscriber::on_subscribed(int number)
{
    if (number <= 0) {
        log_.error("notify [{}]: subscribe answered {}", options_.channel, number);
        arm_retry();
        return;
    }

    // Stopped while the answer was on its way: the number is not used — give it back.
    if (stopped_) {
        number_ = number;
        stop();
        return;
    }

    if (!listening_.empty()) {
        pool_.unlisten(listening_);
        listening_.clear();
    }

    number_    = number;
    listening_ = personal_channel();

    log_.notice("notify [{}]: subscriber {} — listening on \"{}\"", options_.channel, number_, listening_);

    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.listen(listening_,
        [w](std::string_view, std::string_view payload) {
            if (auto p = w.lock())
                (*p)->on_wake(payload);
        },
        // Every confirmation of the LISTEN — the first, and each one on a new
        // listener after a loss: a wake-up sent meanwhile may be lost, the sweep
        // says it again; the heartbeat records the new listener's pid.
        [w](std::string_view) {
            auto p = w.lock();
            if (!p)
                return;
            (*p)->sweep_now();
            (*p)->heartbeat_now();
        });

    if (draining_) {
        // drain() came before the number: say it now.
        pool_.listener_execute(fmt::format("SELECT notify.drain({})", number_), {}, {}, false,
                               answer_within());
    } else {
        arm_sweep();
    }
    arm_heartbeat();

    if (on_number_)
        guarded(log_, options_.channel, "number handler", [&] { on_number_(number_); });
}

// ─── A wake-up → take → the module ───────────────────────────────────────────

void NotifySubscriber::on_wake(std::string_view payload)
{
    if (draining_ || stopped_ || number_ == 0)
        return;

    const auto id = parse_id(payload);
    if (!id) {
        log_.error("notify [{}]: a wake-up that is not a row id: \"{}\"", options_.channel, payload);
        return;
    }

    take(*id);
}

void NotifySubscriber::take(std::int64_t id)
{
    if (taken_.contains(id))
        return;   // with the module already; a second wake-up for it is no news

    const std::string sql = fmt::format(
        "SELECT payload, key, token, attempts FROM notify.take({}, {})", id, number_);

    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.execute(sql,
        [w, id](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (!r || !r->ok()) {
                // The row is still waiting: the sweep wakes somebody for it again.
                self.log_.error("notify [{}]: take {} failed: {}", self.options_.channel, id,
                                r ? r->error_message() : "no result");
                return;
            }
            if (r->rows() == 0)
                return;   // not ours: taken, somebody else's, waiting for an earlier one, closed

            NotifyRow row;
            row.id       = id;
            row.payload  = text_of(*r, 0);
            row.has_key  = !r->is_null(0, 1);
            row.key      = text_of(*r, 1);
            row.token    = std::strtoll(r->value(0, 2), nullptr, 10);
            row.attempts = static_cast<int>(std::strtol(r->value(0, 3), nullptr, 10));

            self.taken_.emplace(id, row);

            if (self.stopped_) {
                self.refuse(row, "stopping: taken after the stop");
                return;
            }

            try {
                self.on_row_(row);
            } catch (const std::exception& e) {
                self.log_.error("notify [{}]: row {} handler threw: {}", self.options_.channel, id, e.what());
                if (self.taken_.contains(id))
                    self.refuse(row, fmt::format("error: {}", e.what()));
            }
        });
}

// ─── The outcome ─────────────────────────────────────────────────────────────

PgPool::QueryId NotifySubscriber::complete(const NotifyRow& row, std::string result_sql,
                                           DoneHandler on_done, FailHandler on_fail, bool quiet)
{
    auto it = taken_.find(row.id);
    if (it == taken_.end() || it->second.token != row.token) {
        // Refused already — by stop() among others —, completed twice, or a row
        // this subscriber did not take: the ack would be refused anyway.
        log_.error("notify [{}]: complete({}) for a row that is not with the module", options_.channel, row.id);
        if (on_fail)
            guarded(log_, options_.channel, "fail handler",
                    [&] { on_fail(Failure{"the row is not with the module", {}}, true); });
        return 0;
    }
    taken_.erase(it);

    const std::int64_t id = row.id;
    std::weak_ptr<NotifySubscriber*> w = self_;
    return pool_.execute(ack_sql(row, result_sql),
        [w, id, on_done = std::move(on_done), on_fail = std::move(on_fail)](std::vector<PgResult> results) {
            // Called whether or not the subscriber is still there; logged when it is.
            auto call = [&w](std::string_view what, auto&& f) {
                try {
                    f();
                } catch (const std::exception& e) {
                    if (auto p = w.lock())
                        (*p)->log_.error("notify [{}]: {} threw: {}", (*p)->options_.channel, what, e.what());
                }
            };
            auto bad = std::find_if(results.begin(), results.end(),
                [](const PgResult& r) { return !r.ok(); });

            if (bad == results.end()) {
                if (!results.empty())
                    results.pop_back();   // the ack's own
                if (on_done)
                    call("done handler", [&] { on_done(std::move(results)); });
                return;
            }

            const bool stale = std::string_view(bad->sqlstate()) == "NT409";
            if (auto p = w.lock()) {
                auto& self = **p;
                if (stale)
                    self.log_.warn("notify [{}]: row {}: the token is not current — the result is rolled back, "
                                   "the row is somebody else's", self.options_.channel, id);
                else
                    self.log_.error("notify [{}]: row {}: result and ack failed ({}): {}", self.options_.channel,
                                    id, *bad->sqlstate() ? bad->sqlstate() : "no answer", bad->error_message());
            }
            if (on_fail)
                call("fail handler", [&] { on_fail(Failure{bad->error_message(), bad->sqlstate()}, stale); });
        },
        {}, quiet);
}

void NotifySubscriber::refuse(const NotifyRow& row, std::string_view reason)
{
    auto it = taken_.find(row.id);
    if (it == taken_.end() || it->second.token != row.token) {
        log_.error("notify [{}]: refuse({}) for a row that is not with the module", options_.channel, row.id);
        return;
    }
    taken_.erase(it);

    const std::int64_t id = row.id;
    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.execute(fmt::format("SELECT notify.nack({}, {}, {})", row.id, row.token, pq_quote_literal(reason)),
        [w, id](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (r && r->ok())
                return;
            if (r && std::string_view(r->sqlstate()) == "NT409")
                self.log_.warn("notify [{}]: nack {}: the token is not current", self.options_.channel, id);
            else
                self.log_.error("notify [{}]: nack {} failed: {}", self.options_.channel, id,
                                r ? r->error_message() : "no result");
        });
}

// ─── Stop ────────────────────────────────────────────────────────────────────

void NotifySubscriber::drain()
{
    if (draining_ || stopped_)
        return;
    draining_ = true;

    cancel(sweep_timer_);
    cancel(retry_timer_);

    if (number_ == 0)
        return;   // subscribing: on_subscribed() says it

    log_.notice("notify [{}]: subscriber {} draining, {} row(s) with the module",
                options_.channel, number_, taken_.size());

    // On the listener: the pool's connections may all be held by work that hangs.
    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.listener_execute(fmt::format("SELECT notify.drain({})", number_),
        [w](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (!r || !r->ok())
                (*p)->log_.error("notify [{}]: drain failed: {}", (*p)->options_.channel,
                                 r ? r->error_message() : "no result");
        },
        {}, false, answer_within());
}

void NotifySubscriber::stop(std::string_view reason)
{
    const bool first = !stopped_;
    stopped_  = true;
    draining_ = true;
    cancel_timers();

    if (!first && number_ == 0)
        return;

    // Only the rows still with the module. One whose completion has been sent
    // is not here (complete() takes it out): its ack is on its way, and a nack
    // ahead of it would return the row and roll back a finished result.
    // Draining first, in the same batch: the drain() of on_stopping may have
    // been lost with a listener, or never sent — and an active subscriber is
    // what nack picks the row's next addressee among.
    std::string sql;
    if (number_ != 0)
        sql += fmt::format("SELECT notify.drain({}); ", number_);

    // Inside a $notify$ body: a reason carrying the tag would end it early.
    std::string why(reason);
    std::erase(why, '$');
    for (const auto& [id, row] : taken_) {
        // A token that is not current any more is no reason to give up the rest.
        sql += fmt::format(
            "DO $notify$ BEGIN PERFORM notify.nack({}, {}, {}); "
            "EXCEPTION WHEN SQLSTATE 'NT409' THEN NULL; END $notify$; ",
            row.id, row.token, pq_quote_literal(why));
    }
    const std::size_t refused = taken_.size();
    taken_.clear();

    if (number_ != 0)
        sql += fmt::format("SELECT notify.unsubscribe({})", number_);

    if (sql.empty())
        return;

    log_.notice("notify [{}]: subscriber {} stopping — {} row(s) refused, deregistering",
                options_.channel, number_, refused);

    // On the listener, as drain(); the drain after on_stop() waits for it.
    const std::string channel = options_.channel;
    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.listener_execute(std::move(sql),
        [w, channel](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto bad = std::find_if(results.begin(), results.end(),
                [](const PgResult& r) { return !r.ok(); });
            if (bad != results.end())
                (*p)->log_.error("notify [{}]: stop failed: {}", channel, bad->error_message());
        },
        {}, false, answer_within());
    number_ = 0;
}

// ─── Timers ──────────────────────────────────────────────────────────────────

void NotifySubscriber::heartbeat_now()
{
    if (number_ == 0 || stopped_ || heartbeat_in_flight_)
        return;
    heartbeat_in_flight_ = true;

    const int number = number_;
    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.listener_execute(fmt::format("SELECT notify.heartbeat({}, pg_backend_pid())", number),
        [w, number](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            self.heartbeat_in_flight_ = false;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (!r || !r->ok() || r->rows() != 1) {
                self.log_.error("notify [{}]: heartbeat failed: {}", self.options_.channel,
                                r ? r->error_message() : "no result");
                return;
            }
            if (number != self.number_ || self.stopped_)
                return;
            if (r->is_null(0, 0)) {
                // The database does not know the number any more: a new one.
                self.log_.warn("notify [{}]: subscriber {} is not registered any more — subscribing again",
                               self.options_.channel, number);
                self.number_ = 0;
                if (self.on_number_)
                    guarded(self.log_, self.options_.channel, "number handler", [&] { self.on_number_(0); });
                self.subscribe();
                return;
            }
            if (!self.draining_ && std::string_view(r->value(0, 0)) == "draining")
                self.log_.warn("notify [{}]: subscriber {} is draining, not by this process",
                               self.options_.channel, number);
        },
        [w](std::string_view error) {
            auto p = w.lock();
            if (!p)
                return;
            (*p)->heartbeat_in_flight_ = false;
            (*p)->log_.error("notify [{}]: heartbeat failed: {}", (*p)->options_.channel, error);
        },
        /*quiet=*/true, answer_within());
}

void NotifySubscriber::sweep_now()
{
    if (number_ == 0 || draining_ || stopped_ || sweep_in_flight_)
        return;
    sweep_in_flight_ = true;

    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.execute(fmt::format("SELECT notify.sweep({}, {})", number_, options_.sweep_limit),
        [w](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            self.sweep_in_flight_ = false;
            const PgResult* r = results.empty() ? nullptr : &results.back();
            if (!r || !r->ok())
                self.log_.error("notify [{}]: sweep failed: {}", self.options_.channel,
                                r ? r->error_message() : "no result");
        },
        {}, /*quiet=*/true);
}

void NotifySubscriber::read_policy()
{
    if (policy_in_flight_ || stopped_)
        return;
    policy_in_flight_ = true;

    const std::string sql = fmt::format(
        "SELECT mode, classified, affinity, ceil(extract(epoch FROM alive))::int, "
        "ceil(extract(epoch FROM sweep_every))::int, ceil(extract(epoch FROM lease))::int "
        "FROM notify.channel_policy({})", pq_quote_literal(options_.channel));

    std::weak_ptr<NotifySubscriber*> w = self_;
    pool_.execute(sql,
        [w](std::vector<PgResult> results) {
            auto p = w.lock();
            if (!p)
                return;
            auto& self = **p;
            self.policy_in_flight_ = false;
            const PgResult* r = results.empty() ? nullptr : &results.back();

            std::string problem;
            if (!r || !r->ok()) {
                problem = fmt::format("the channel policy is not readable: {}", r ? r->error_message() : "no result");
            } else if (r->rows() == 0) {
                problem = "the channel is not in notify.channel";
            } else {
                const std::string mode = text_of(*r, 0);
                const bool classified  = text_of(*r, 1) == "t";

                const auto heartbeat_was = self.heartbeat_every();
                const auto sweep_was     = self.sweep_every();
                self.alive_        = seconds_of(*r, 3, std::chrono::seconds{0});
                self.policy_sweep_ = seconds_of(*r, 4, std::chrono::seconds{0});
                if (self.heartbeat_every() != heartbeat_was && self.heartbeat_timer_ != EventLoop::kInvalidTimer)
                    self.arm_heartbeat();
                if (self.sweep_every() != sweep_was && self.sweep_timer_ != EventLoop::kInvalidTimer)
                    self.arm_sweep();

                if (!classified)
                    problem = fmt::format("the channel is not classified (mode \"{}\" recorded on first sight)", mode);
                else if (mode != self.options_.expected_mode && mode != "legacy")
                    problem = fmt::format("the channel is in mode \"{}\", this module is written for \"{}\"",
                                          mode, self.options_.expected_mode);
            }

            if (problem.empty()) {
                if (!self.policy_ok_)
                    self.log_.notice("notify [{}]: channel policy matches", self.options_.channel);
                self.policy_ok_ = true;
                self.policy_problem_.clear();
            } else {
                // At every check, not once: a mismatch nobody reads is no check.
                self.log_.error("notify [{}]: {}", self.options_.channel, problem);
                self.policy_ok_      = false;
                self.policy_problem_ = std::move(problem);
            }
        },
        {}, /*quiet=*/true);
}

std::chrono::seconds NotifySubscriber::heartbeat_every() const
{
    if (options_.heartbeat.count() > 0)
        return options_.heartbeat;
    if (alive_.count() > 0)
        return std::max(std::chrono::seconds{1}, alive_ / 3);
    return kDefaultHeartbeat;
}

std::chrono::seconds NotifySubscriber::sweep_every() const
{
    if (options_.sweep.count() > 0)
        return options_.sweep;
    if (policy_sweep_.count() > 0)
        return policy_sweep_;
    return kDefaultSweep;
}

std::chrono::seconds NotifySubscriber::answer_within() const
{
    if (options_.answer_within.count() > 0)
        return options_.answer_within;
    if (alive_.count() > 0)
        return alive_;
    return kDefaultAlive;
}

void NotifySubscriber::cancel(EventLoop::TimerId& id)
{
    if (id != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(id);
        id = EventLoop::kInvalidTimer;
    }
}

void NotifySubscriber::cancel_timers()
{
    cancel(heartbeat_timer_);
    cancel(sweep_timer_);
    cancel(policy_timer_);
    cancel(retry_timer_);
}

void NotifySubscriber::arm_heartbeat()
{
    cancel(heartbeat_timer_);
    if (stopped_)
        return;
    std::weak_ptr<NotifySubscriber*> w = self_;
    heartbeat_timer_ = loop_.add_timer(heartbeat_every(), [w] {
        auto p = w.lock();
        if (!p)
            return;
        (*p)->heartbeat_timer_ = EventLoop::kInvalidTimer;
        (*p)->heartbeat_now();
        (*p)->arm_heartbeat();
    }, /*repeat=*/false);
}

void NotifySubscriber::arm_sweep()
{
    cancel(sweep_timer_);
    if (draining_ || stopped_)
        return;
    // Spread ±25 %: every subscriber of a channel sweeps it, and in step their
    // sweeps would all land on the database at the same instant. A row is not
    // woken again sooner than 3/4 of sweep_every whoever sweeps (the database
    // keeps the mark), so the spread costs no extra wake-ups.
    const auto base = std::chrono::duration_cast<std::chrono::milliseconds>(sweep_every());
    std::uniform_int_distribution<long> d(base.count() * 3 / 4, base.count() * 5 / 4);
    const auto delay = std::chrono::milliseconds(std::max(1L, d(spread_)));

    std::weak_ptr<NotifySubscriber*> w = self_;
    sweep_timer_ = loop_.add_timer(delay, [w] {
        auto p = w.lock();
        if (!p)
            return;
        (*p)->sweep_timer_ = EventLoop::kInvalidTimer;
        (*p)->sweep_now();
        (*p)->arm_sweep();
    }, /*repeat=*/false);
}

void NotifySubscriber::arm_policy()
{
    cancel(policy_timer_);
    if (stopped_ || options_.policy_every.count() <= 0)
        return;
    std::weak_ptr<NotifySubscriber*> w = self_;
    policy_timer_ = loop_.add_timer(options_.policy_every, [w] {
        auto p = w.lock();
        if (!p)
            return;
        (*p)->policy_timer_ = EventLoop::kInvalidTimer;
        (*p)->read_policy();
        (*p)->arm_policy();
    }, /*repeat=*/false);
}

void NotifySubscriber::arm_retry()
{
    cancel(retry_timer_);
    if (draining_ || stopped_)
        return;
    std::weak_ptr<NotifySubscriber*> w = self_;
    retry_timer_ = loop_.add_timer(std::max(std::chrono::seconds{1}, options_.retry), [w] {
        auto p = w.lock();
        if (!p)
            return;
        (*p)->retry_timer_ = EventLoop::kInvalidTimer;
        (*p)->subscribe();
    }, /*repeat=*/false);
}

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
