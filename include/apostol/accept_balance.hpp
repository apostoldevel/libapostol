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
//
// Connections may queue on the socket before the workers run: the master
// listens before it forks. The master therefore sets a slot aside for each
// worker before forking it, counted as holding nothing for kStartGraceMs —
// the worker that runs first leaves the queue to the others while they start
// instead of taking all of it.

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
    // How long a slot set aside for a starting worker counts before the
    // worker says anything itself.
    static constexpr std::chrono::milliseconds kStartGraceMs{1000};
    // reserve() dates the slot kStaleMs - kStartGraceMs back.
    static_assert(kStartGraceMs > std::chrono::milliseconds{0} && kStartGraceMs < kStaleMs);
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

    /// Master, before it forks a worker: a free slot is set aside for it,
    /// counted as holding nothing for kStartGraceMs. False — none free: the
    /// worker takes one itself in join().
    bool reserve();
    /// Master, after fork: the slot set aside by reserve() is @p pid's — freed
    /// by release(@p pid). @p pid <= 0 (fork failed): it is free again. Nothing
    /// set aside — nothing done.
    void reserved_for(pid_t pid);

    /// Worker: take the slot the master set aside for it before the fork, or
    /// else a free one, for @p pid. False — none: no balancing. A slot taken
    /// here is not counted by the others until the first publish(); one set
    /// aside is counted from reserve(), for kStartGraceMs at most.
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
    int       reserved_{-1};        // master: set aside for the next fork;
                                    // the forked worker: its own
    long long window_ms_{0};        // the current kChurnMs window began
    int       closes_{0};           // short closes in it
    int       closes_before_{0};    // short closes in the window before
};

} // namespace apostol
