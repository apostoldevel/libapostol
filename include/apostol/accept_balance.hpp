#pragma once

// AcceptBalance — spreads the connections of one listening socket shared by
// several workers.
//
// Every worker that shares the master's socket is woken by the same arrivals;
// the one that runs first takes them, and a burst — every client reconnecting
// at once — lands on one worker. A table in shared memory, made by the master
// before it forks, holds one slot per worker: its pid, how many accepted
// connections it holds, and when it last said so. A worker holding more than
// the least loaded of the others leaves a new connection to them for a while;
// a worker that has not said anything for kStaleMs is not counted, and a
// worker that has waited kDeferMs takes one connection anyway and waits again
// — nothing waits for long on a worker that does not accept.

#include <sys/types.h>

#include <chrono>
#include <cstddef>

namespace apostol
{

class AcceptBalance
{
public:
    static constexpr std::size_t kSlots = 256;
    static constexpr std::chrono::milliseconds kStaleMs{3000};
    static constexpr std::chrono::milliseconds kDeferMs{100};
    static constexpr std::chrono::milliseconds kRecheckMs{1};
    // A wait not looked at again for this long has ended (the queue emptied):
    // the next one starts afresh.
    static constexpr std::chrono::milliseconds kEpisodeGapMs{50};
    // A worker closing kChurnCloses short connections (open less than
    // kChurnMs) or more per kChurnMs takes what comes: short connections come
    // and go faster than a wait, and waiting on them only idles the worker.
    // Long ones are what is spread — also when many of them close at once
    // (a proxy in front restarts) and their clients come back together.
    static constexpr std::chrono::milliseconds kChurnMs{1000};
    static constexpr int kChurnCloses = 20;

    AcceptBalance() = default;
    ~AcceptBalance() = default;

    AcceptBalance(const AcceptBalance&)            = delete;
    AcceptBalance& operator=(const AcceptBalance&) = delete;

    /// Master, before fork. False — shared memory refused: no balancing.
    bool create();
    bool created() const noexcept { return slots_ != nullptr; }

    /// Worker: take a free slot for @p pid. False — none: no balancing. The
    /// slot is not counted by the others until the first publish().
    bool join(pid_t pid);
    /// Worker: the slot is given up — it accepts nothing any more.
    void leave();
    bool joined() const noexcept { return mine_ >= 0; }

    /// Worker: @p load accepted connections held now; also says it is alive.
    void publish(int load);
    /// Worker: a connection open for @p lived_ms has closed.
    void closed(long long lived_ms);

    /// Worker: true — take the next connection with @p load held now; false —
    /// another live worker holds fewer, leave it to them.
    bool may_accept(int load) const;

    /// Master: @p pid reaped — its slot is free.
    void release(pid_t pid);

private:
    struct Slot;
    Slot*     slots_{nullptr};
    int       mine_{-1};
    long long window_ms_{0};        // the current kChurnMs window began
    int       closes_{0};           // short closes in it
    int       closes_before_{0};    // short closes in the window before
};

} // namespace apostol
