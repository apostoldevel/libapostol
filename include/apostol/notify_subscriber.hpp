#pragma once

#if defined(WITH_POSTGRESQL) && defined(WITH_DB_PLATFORM)

#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/pg.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace apostol::db_platform
{

// ─── NotifySubscriber ────────────────────────────────────────────────────────
//
// A listener of a db-platform `notify` channel in mode `single`: a NOTIFY there
// is not a broadcast that every copy of the application acts on, but a row of
// notify.queue handed to exactly one live subscriber, taken under a token and
// confirmed with that token in the same transaction as the result. A late
// confirmation under an old token is refused, and the result written beside it
// is rolled back with it.
//
// One instance per (process, channel). What it does, in the database's terms:
//
//   start()      notify.channel_policy — the mode to check against, the timers;
//                notify.subscribe ON THE LISTENER — the number, and the
//                listener's pid; LISTEN "<channel>:<number>"; after every
//                confirmation of that LISTEN (the first one, a reconnect)
//                notify.sweep and notify.heartbeat at once.
//   a wake-up    "<channel>:<number>" carries a row id → notify.take → the row
//                handler, with payload, key, token and attempts. Nothing taken
//                (somebody else's, already taken, closed) — nothing happens.
//   complete()   the module's result and notify.ack in ONE statement batch — one
//                implicit transaction. SQLSTATE NT409: the token is not current
//                any more, the result was rolled back, the row is somebody
//                else's now.
//   refuse()     notify.nack — given out again if a repeat is allowed.
//   timers       notify.heartbeat ON THE LISTENER (a listener the network cut
//                silently answers nothing: it is replaced, and its subscriber
//                is not chosen meanwhile), NULL → subscribe anew;
//                notify.sweep on the pool, with a spread; channel_policy again.
//   drain()      for on_stopping(): notify.drain (not chosen as an addressee
//                any more), nothing new taken; rows taken go on to their end.
//   stop()       for on_stop(): rows still with the module — taken, neither
//                completed nor refused — are refused with "stopping: …", and
//                the subscriber is deregistered, in one batch on the listener.
//                A row whose completion has been sent is left alone: its ack is
//                on its way.
//
// The listener carries subscribe, heartbeat, drain and stop because the pool's
// connections may all be held by work that hangs; a deregistration queued
// behind them would not get out.
//
// The common channel "<channel>" is listened to only when the module gives a
// legacy handler: it carries the producers' own data while the channel is in
// mode `legacy` (a rollout), never a wake-up.
//
// The mode check: the module says which mode it is written for. No channel, a
// channel nobody classified, or another mode — an error in the log at every
// check and policy_ok() false; `legacy` is not a mismatch (a rollout). The
// subscriber goes on working either way.

struct NotifyRow
{
    std::int64_t id       = 0;
    std::string  payload;
    std::string  key;            ///< empty when the producer gave none (has_key false)
    bool         has_key  = false;
    std::int64_t token    = 0;
    int          attempts = 0;   ///< deliveries so far, this one included
};

class NotifySubscriber
{
public:
    struct Options
    {
        std::string channel;
        /// The mode the module is written for.
        std::string expected_mode{"single"};
        /// Who this is, for people: notify.subscriber and the listener's
        /// application_name ("<application> <role> <node>").
        std::string node;
        std::string application;
        std::string role;
        /// Zero — from the channel's policy: a third of `alive`, at least 1 s.
        std::chrono::seconds heartbeat{0};
        /// Zero — the channel's sweep_every. Each tick is spread by ±25 %.
        std::chrono::seconds sweep{0};
        /// How often the policy is read again (the mode check).
        std::chrono::seconds policy_every{30};
        /// Zero — the channel's `alive`: a listener that does not answer its
        /// heartbeat in that time is replaced.
        std::chrono::seconds answer_within{0};
        /// After a failed subscribe.
        std::chrono::seconds retry{5};
        int                  sweep_limit{100};
    };

    /// A row to work on. The module calls complete() or refuse() for it, once.
    using RowHandler    = std::function<void(const NotifyRow&)>;
    /// A NOTIFY on the common channel: the producer's data (mode `legacy`).
    using LegacyHandler = std::function<void(std::string_view payload)>;
    /// The subscriber number changed: the first subscription, and a new one
    /// after the database forgot the old (heartbeat answered NULL). Zero — none
    /// right now. A module that publishes its number (a route) publishes again.
    using NumberHandler = std::function<void(int number)>;

    /// complete(): the results of the module's statement (without the ack's).
    using DoneHandler   = std::function<void(std::vector<PgResult>)>;
    /// What complete() tells a module that did not get its confirmation. It
    /// converts to the text, so a handler taking (std::string_view error,
    /// bool stale) is a FailHandler as well.
    struct Failure
    {
        std::string error;
        /// The SQLSTATE of the statement that failed. An error of the server's
        /// own: the batch is one transaction, and nothing of it was written
        /// ("40001", "40P01", "55P03" are worth a repeat; "NT409" is stale).
        /// Class 08 and 57P0x say the connection or the server went (a
        /// pooler in front reports a lost server this way): written or not
        /// is unknown. "" — no answer from the server: the batch was lost
        /// with the connection (written or not is unknown) or never sent; with
        /// stale, the row was not with the module and nothing was sent.
        /// A repeat is the module's own: complete() is once per row (a second
        /// one finds the row gone and says stale) — send ack_sql() through the
        /// pool; a repeat of a batch that was written is refused as NT409.
        std::string sqlstate;

        operator std::string_view() const noexcept { return error; }
    };

    /// complete() did not confirm. @p stale — the token was not current: the
    /// result is rolled back and the row is somebody else's. Otherwise the
    /// statement failed or the outcome is unknown — failure.sqlstate tells
    /// which; the row returns by its lease.
    using FailHandler   = std::function<void(const Failure& failure, bool stale)>;

    /// @p pool must outlive the subscriber.
    NotifySubscriber(PgPool& pool, EventLoop& loop, Logger& log, Options options,
                     RowHandler on_row, LegacyHandler on_legacy = {},
                     NumberHandler on_number = {});
    ~NotifySubscriber();

    NotifySubscriber(const NotifySubscriber&)            = delete;
    NotifySubscriber& operator=(const NotifySubscriber&) = delete;

    void start();

    /// The result and the confirmation, in one transaction: "<result_sql>
    /// ; SELECT notify.ack(id, token)" (the ack on a line of its own). @p result_sql may be empty (nothing to
    /// write but the confirmation) and may hold several statements; it must not
    /// commit or open a transaction of its own.
    ///
    /// Returns the pool's id of the batch, sent or queued, for
    /// PgPool::cancel(); 0 when nothing was sent (the row is not with the
    /// module, @p on_fail is told). A batch canceled before it finished calls
    /// neither handler, whatever cancel() returned, and whether the row was
    /// confirmed is not known: a cancel in time left nothing written — the row
    /// returns by its lease (refuse() is not possible any more) —, one too
    /// late found the result and the ack committed. cancel() of a batch that
    /// has finished does nothing (and still returns true): a module that may
    /// cancel late keeps its own flag, set by the handlers.
    PgPool::QueryId complete(const NotifyRow& row, std::string result_sql,
                             DoneHandler on_done = {}, FailHandler on_fail = {},
                             bool quiet = false);

    /// notify.nack: the module will not do it. @p reason goes into the row.
    void refuse(const NotifyRow& row, std::string_view reason);

    /// For on_stopping().
    void drain();

    /// Rows with the module: taken, neither completed nor refused. The
    /// statements already sent are the pool's to count.
    bool busy() const { return !taken_.empty(); }

    /// For on_stop(). @p reason starts with "stopping:".
    void stop(std::string_view reason = "stopping: the process stopped before the work was finished");

    int  number()    const { return number_; }
    bool policy_ok() const { return policy_ok_; }
    /// Why policy_ok() is false; empty when it is true.
    const std::string& policy_problem() const { return policy_problem_; }

    const std::string& channel() const { return options_.channel; }

    /// The statement of complete(), for a module that sends it itself: the
    /// result followed by the confirmation.
    static std::string ack_sql(const NotifyRow& row, std::string_view result_sql = {});

private:
    void subscribe();
    void on_subscribed(int number);
    void on_wake(std::string_view payload);
    void take(std::int64_t id);
    void heartbeat_now();
    void sweep_now();
    void read_policy();

    void arm_heartbeat();
    void arm_sweep();
    void arm_policy();
    void arm_retry();
    void cancel(EventLoop::TimerId& id);
    void cancel_timers();

    std::chrono::seconds heartbeat_every() const;
    std::chrono::seconds sweep_every() const;
    std::chrono::seconds answer_within() const;

    std::string personal_channel() const;

    PgPool&    pool_;
    EventLoop& loop_;
    Logger&    log_;
    Options    options_;

    RowHandler    on_row_;
    LegacyHandler on_legacy_;
    NumberHandler on_number_;

    // Callbacks the pool and the loop hold carry a weak copy: a subscriber
    // destroyed first is not called into.
    std::shared_ptr<NotifySubscriber*> self_;

    int         number_{0};
    std::string listening_;          // the personal channel listened to; empty — none

    // id → row: with the module (taken, no complete()/refuse() yet).
    std::map<std::int64_t, NotifyRow> taken_;

    bool started_{false};
    bool subscribing_{false};
    bool draining_{false};
    bool stopped_{false};
    bool heartbeat_in_flight_{false};
    bool sweep_in_flight_{false};
    bool policy_in_flight_{false};

    // From the channel's policy; zero until it is read.
    std::chrono::seconds alive_{0};
    std::chrono::seconds policy_sweep_{0};
    bool                 policy_ok_{false};
    std::string          policy_problem_{"the channel policy is not read yet"};

    EventLoop::TimerId heartbeat_timer_{EventLoop::kInvalidTimer};
    EventLoop::TimerId sweep_timer_{EventLoop::kInvalidTimer};
    EventLoop::TimerId policy_timer_{EventLoop::kInvalidTimer};
    EventLoop::TimerId retry_timer_{EventLoop::kInvalidTimer};

    std::minstd_rand spread_;
};

} // namespace apostol::db_platform

#endif // WITH_POSTGRESQL && WITH_DB_PLATFORM
