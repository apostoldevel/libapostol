#include "apostol/node_directory.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace apostol
{

// Every field a reader looks at is atomic. The name is plain bytes: it is
// written only while the slot is free (nobody reads it then) and published by
// the release store of the state that follows.
struct NodeDirectory::Slot
{
    std::atomic<std::uint8_t>  state{0};
    std::atomic<std::uint8_t>  role{0};
    std::atomic<std::int32_t>  pid{0};
    std::atomic<std::int32_t>  number{0};
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::int64_t>  seen_ms{0};
    std::atomic<std::int64_t>  since_ms{0};
    char                       name[kNameMax + 1]{};
};

static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);

long long NodeDirectory::now_ms() noexcept
{
    // CLOCK_MONOTONIC: one clock for every process of the host.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool NodeDirectory::create()
{
    if (slots_)
        return true;
    void* p = ::mmap(nullptr, sizeof(Slot) * kSlots, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;
    slots_ = static_cast<Slot*>(p);
    for (std::size_t i = 0; i < kSlots; ++i)
        new (&slots_[i]) Slot{};
    return true;
}

int NodeDirectory::reserve(ProcessRole role, int number, std::string_view name,
                           std::uint32_t generation)
{
    if (!slots_)
        return -1;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        Slot& s = slots_[i];
        if (s.state.load(std::memory_order_acquire) != static_cast<std::uint8_t>(State::free))
            continue;
        s.generation.store(generation, std::memory_order_relaxed);
        s.role.store(static_cast<std::uint8_t>(role), std::memory_order_relaxed);
        s.number.store(number, std::memory_order_relaxed);
        s.pid.store(0, std::memory_order_relaxed);
        s.seen_ms.store(0, std::memory_order_relaxed);
        s.since_ms.store(now_ms(), std::memory_order_relaxed);
        const std::size_t n = std::min(name.size(), kNameMax);
        std::memcpy(s.name, name.data(), n);
        s.name[n] = '\0';
        s.state.store(static_cast<std::uint8_t>(State::starting), std::memory_order_release);
        return static_cast<int>(i);
    }
    return -1;
}

void NodeDirectory::assign(int slot, pid_t pid)
{
    if (!slots_ || slot < 0 || static_cast<std::size_t>(slot) >= kSlots)
        return;
    slots_[slot].pid.store(pid, std::memory_order_release);
}

void NodeDirectory::retire(int slot)
{
    mark(slot, State::retiring);
}

void NodeDirectory::release(int slot)
{
    if (!slots_ || slot < 0 || static_cast<std::size_t>(slot) >= kSlots)
        return;
    Slot& s = slots_[slot];
    s.since_ms.store(now_ms(), std::memory_order_relaxed);
    s.state.store(static_cast<std::uint8_t>(State::free), std::memory_order_release);
}

void NodeDirectory::mark(int slot, State state)
{
    if (!slots_ || slot < 0 || static_cast<std::size_t>(slot) >= kSlots || state == State::free)
        return;
    Slot& s = slots_[slot];
    auto cur = s.state.load(std::memory_order_acquire);
    // Never resurrect: a retiring process stays retiring, a freed slot free.
    while (cur != static_cast<std::uint8_t>(State::free)
           && cur != static_cast<std::uint8_t>(State::retiring)
           && cur != static_cast<std::uint8_t>(state))
    {
        if (s.state.compare_exchange_weak(cur, static_cast<std::uint8_t>(state),
                                          std::memory_order_acq_rel))
        {
            s.since_ms.store(now_ms(), std::memory_order_relaxed);
            break;
        }
    }
    if (state == State::live)
        stamp(slot);
}

void NodeDirectory::stamp(int slot)
{
    if (!slots_ || slot < 0 || static_cast<std::size_t>(slot) >= kSlots)
        return;
    slots_[slot].seen_ms.store(now_ms(), std::memory_order_relaxed);
}

std::optional<NodeDirectory::Entry> NodeDirectory::get(int slot) const
{
    if (!slots_ || slot < 0 || static_cast<std::size_t>(slot) >= kSlots)
        return std::nullopt;
    const Slot& s = slots_[slot];
    const auto gen = s.generation.load(std::memory_order_acquire);
    const auto st  = s.state.load(std::memory_order_acquire);
    if (st == static_cast<std::uint8_t>(State::free))
        return std::nullopt;

    Entry e;
    e.slot       = slot;
    e.state      = static_cast<State>(st);
    e.role       = static_cast<ProcessRole>(s.role.load(std::memory_order_relaxed));
    e.number     = s.number.load(std::memory_order_relaxed);
    e.pid        = s.pid.load(std::memory_order_acquire);
    e.generation = gen;
    e.seen_ms    = s.seen_ms.load(std::memory_order_relaxed);
    e.since_ms   = s.since_ms.load(std::memory_order_relaxed);
    char name[kNameMax + 1];
    std::memcpy(name, s.name, sizeof(name));
    name[kNameMax] = '\0';
    e.name = name;

    // Reused while read (freed and taken again): the fields may mix two
    // processes — report nothing rather than a hybrid. The fence keeps the
    // reads above before the re-check on a weakly ordered CPU (a seqlock
    // read side); the plain copy of the name may still race a writer, the
    // re-check is what discards such a copy.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.generation.load(std::memory_order_acquire) != gen
        || s.state.load(std::memory_order_acquire) == static_cast<std::uint8_t>(State::free))
        return std::nullopt;
    return e;
}

std::vector<NodeDirectory::Entry> NodeDirectory::list() const
{
    std::vector<Entry> out;
    if (!slots_)
        return out;
    for (std::size_t i = 0; i < kSlots; ++i)
        if (auto e = get(static_cast<int>(i)))
            out.push_back(std::move(*e));
    return out;
}

} // namespace apostol
