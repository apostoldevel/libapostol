#include "apostol/accept_balance.hpp"

#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <new>

namespace apostol
{

// Atomics in memory shared by processes: only lock-free ones are address-free.
struct AcceptBalance::Slot
{
    std::atomic<std::int32_t> pid{0};
    std::atomic<std::int32_t> load{0};
    std::atomic<std::int64_t> seen_ms{0};
};

static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);

namespace
{

// A slot set aside by the master for a worker not forked yet. Not 0 — a slot
// with pid 0 is free — and no pid, so release() never matches it.
constexpr std::int32_t kReserved = -1;

std::int64_t now_ms() noexcept
{
    // CLOCK_MONOTONIC is one clock for every process on the host.
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

bool AcceptBalance::create()
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

bool AcceptBalance::reserve()
{
    // One set aside earlier and never handed to a pid would be lost for good:
    // release() matches no pid in it, join() takes only free slots.
    if (reserved_ >= 0)
        reserved_for(-1);
    if (!slots_)
        return false;
    for (std::size_t i = 0; i < kSlots; ++i) {
        std::int32_t free = 0;
        if (slots_[i].pid.compare_exchange_strong(free, kReserved)) {
            // A free slot reads seen_ms 0 — not counted — until this store.
            slots_[i].load.store(0);
            slots_[i].seen_ms.store(now_ms() - (kStaleMs - kStartGraceMs).count());
            reserved_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

void AcceptBalance::reserved_for(pid_t pid)
{
    if (!slots_ || reserved_ < 0)
        return;
    Slot& slot = slots_[reserved_];
    reserved_ = -1;
    std::int32_t expected = kReserved;
    if (pid > 0) {
        // The worker may have written its pid first (join): then it stays.
        slot.pid.compare_exchange_strong(expected, static_cast<std::int32_t>(pid));
    } else if (slot.pid.load() == kReserved) {
        slot.seen_ms.store(0);
        slot.pid.compare_exchange_strong(expected, 0);
    }
}

bool AcceptBalance::join(pid_t pid)
{
    if (!slots_ || pid <= 0)
        return false;
    if (mine_ >= 0)
        return true;
    if (reserved_ >= 0) {
        // The slot the master set aside before the fork: its load 0 and its
        // start grace stand — the others count it until the first publish().
        const int i = reserved_;
        reserved_   = -1;
        std::int32_t expected = kReserved;
        if (slots_[i].pid.compare_exchange_strong(expected, static_cast<std::int32_t>(pid))
            || expected == static_cast<std::int32_t>(pid)) {
            mine_ = i;
            return true;
        }
        // Not ours any more (freed meanwhile): a free one, as below.
    }
    for (std::size_t i = 0; i < kSlots; ++i) {
        std::int32_t free = 0;
        if (slots_[i].pid.compare_exchange_strong(free, pid)) {
            slots_[i].load.store(0);
            slots_[i].seen_ms.store(0);     // not counted until publish()
            mine_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

void AcceptBalance::leave()
{
    if (!slots_ || mine_ < 0)
        return;
    slots_[mine_].seen_ms.store(0);
    slots_[mine_].pid.store(0);
    mine_ = -1;
}

void AcceptBalance::publish(int load)
{
    if (!slots_ || mine_ < 0)
        return;
    slots_[mine_].load.store(load);
    slots_[mine_].seen_ms.store(now_ms());
}

void AcceptBalance::closed(long long lived_ms)
{
    if (!slots_ || mine_ < 0 || lived_ms < 0 || lived_ms >= kChurnMs.count())
        return;
    const long long now = now_ms();
    if (now - window_ms_ >= kChurnMs.count()) {
        closes_before_ = now - window_ms_ < 2 * kChurnMs.count() ? closes_ : 0;
        closes_        = 0;
        window_ms_     = now;
    }
    ++closes_;
}

bool AcceptBalance::may_accept(int load) const
{
    if (!slots_ || mine_ < 0)
        return true;
    // Short connections are not waited on (kChurnCloses); long ones go to the
    // least loaded.
    const std::int64_t now = now_ms();
    if (now - window_ms_ < 2 * kChurnMs.count()
        && (closes_ >= kChurnCloses || (now - window_ms_ < kChurnMs.count() && closes_before_ >= kChurnCloses)))
        return true;
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (static_cast<int>(i) == mine_ || slots_[i].pid.load() == 0)
            continue;
        if (now - slots_[i].seen_ms.load() > kStaleMs.count())
            continue;
        if (slots_[i].load.load() < load)
            return false;
    }
    return true;
}

void AcceptBalance::release(pid_t pid)
{
    if (!slots_ || pid <= 0)
        return;
    for (std::size_t i = 0; i < kSlots; ++i) {
        std::int32_t expected = pid;
        if (slots_[i].pid.load() == pid) {
            slots_[i].seen_ms.store(0);
            slots_[i].pid.compare_exchange_strong(expected, 0);
        }
    }
}

} // namespace apostol
