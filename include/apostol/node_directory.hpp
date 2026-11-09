#pragma once

// NodeDirectory — the processes of one node, in shared memory.
//
// A node is the master and the children it forks: workers, the helper and the
// custom processes. The master makes the table before its first fork and keeps
// it for its whole life; a reload does not replace it. One slot per process:
// pid, role, number within the role (worker#3 → 3; 0 for the others), a
// generation that grows with every fork, a state, and when the process last
// said it is alive.
//
// Only the master takes and frees slots: it takes one before fork, so that it
// knows whom to hand the new process's channel to (see Ipc), and frees it when
// it reaps the process. The process itself marks the slot live once it has
// started, and stamps it every second from its own loop.
//
// Readers in any process see a consistent entry or none: the generation is
// checked again after the fields are read, so a slot reused under a reader is
// not mistaken for the process it held before.

#include "apostol/process.hpp"

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

class NodeDirectory
{
public:
    static constexpr std::size_t kSlots   = 256;
    static constexpr std::size_t kNameMax = 63;

    enum class State : std::uint8_t
    {
        free,
        starting,   // taken by the master, the process has not started yet
        live,       // started; a target of gather/broadcast
        retiring,   // told to stop (reload, shutdown); answers a request until
                    // it detaches its channels on the way out
    };

    struct Entry
    {
        int           slot{-1};
        State         state{State::free};
        ProcessRole   role{ProcessRole::worker};
        int           number{0};
        pid_t         pid{0};
        std::uint32_t generation{0};
        long long     seen_ms{0};    // steady clock, ms; 0 — never stamped
        long long     since_ms{0};   // when the state last changed
        std::string   name;
    };

    NodeDirectory() = default;
    ~NodeDirectory() = default;

    NodeDirectory(const NodeDirectory&)            = delete;
    NodeDirectory& operator=(const NodeDirectory&) = delete;

    /// Master, before the first fork. False — shared memory refused.
    bool create();
    bool created() const noexcept { return slots_ != nullptr; }

    /// Master: take a free slot for a process about to be forked. -1 — full.
    int  reserve(ProcessRole role, int number, std::string_view name,
                 std::uint32_t generation);
    /// Master: the process forked into @p slot has pid @p pid.
    void assign(int slot, pid_t pid);
    /// Master: the process in @p slot was told to stop.
    void retire(int slot);
    /// Master: the process in @p slot was reaped — the slot is free.
    void release(int slot);

    /// The process itself: it has started (state live), or stops (retiring).
    void mark(int slot, State state);
    /// The process itself: still alive. Called every second from its loop.
    void stamp(int slot);

    /// A copy of @p slot; nullopt — free, out of range, or reused while read.
    std::optional<Entry> get(int slot) const;
    /// Every slot not free.
    std::vector<Entry> list() const;

    static long long now_ms() noexcept;

private:
    struct Slot;
    Slot* slots_{nullptr};
};

} // namespace apostol
