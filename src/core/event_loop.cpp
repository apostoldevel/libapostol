#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"

#include <atomic>
#include <cerrno>
#include <fmt/format.h>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <system_error>

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace apostol
{

EventLoop::EventLoop()
{
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::system_error(errno, std::system_category(), "epoll_create1");

    sigemptyset(&signal_mask_);
}

EventLoop::~EventLoop()
{
    for (auto& [id, entry] : timers_)
    {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, entry.fd, nullptr);
        ::close(entry.fd);
    }

    if (signal_fd_ >= 0)
    {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, signal_fd_, nullptr);
        ::close(signal_fd_);
    }

    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void EventLoop::run()
{
    running_ = true;

    while (running_)
        poll_once(-1);
}

bool EventLoop::run_for(std::chrono::milliseconds duration,
                        const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;

    // Saved and restored, not forced to false on the way out: this is a public
    // method, and calling it from inside a callback of run() would otherwise stop
    // the outer loop and take the process down with it.
    const bool was_running = running_;

    running_ = true;

    while (running_)
    {
        if (done && done()) {
            running_ = was_running;
            return true;
        }

        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());

        if (left.count() <= 0)
            break;

        poll_once(static_cast<int>(
            std::min<std::int64_t>(left.count(), std::numeric_limits<int>::max())));
    }

    running_ = was_running;
    return done && done();
}

void EventLoop::poll_once(int timeout_ms)
{
    epoll_event events[MAX_EVENTS];

    {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        if (n < 0)
        {
            if (errno == EINTR)
                return;
            throw std::system_error(errno, std::system_category(), "epoll_wait");
        }

        // The batch is keyed by bare fd numbers. A handler earlier in it may
        // close a number and register a new socket or timer under the same one
        // (accept, connect, add_timer); the event still ahead in the batch was
        // raised for the old descriptor and would reach the new holder — a
        // peer's reset read as "connected" by a TcpClient still in SYN_SENT, a
        // fresh timer fired at once (T619). Such events are dropped: nothing is
        // lost, EPOLL_CTL_ADD reports the new descriptor's own readiness to the
        // next epoll_wait, and a new timer has its own expiry. A live socket
        // taken off and registered again by another handler (curl does) is
        // deferred the same way, one epoll_wait later. Held as a number,
        // not a flag, so a nested run_for() cannot hide its registrations.
        const uint64_t collected_at = registration_seq_;

        for (int i = 0; i < n && running_; ++i)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == signal_fd_)
            {
                dispatch_signals();
            }
            else if (auto it = timer_fd_to_id_.find(fd); it != timer_fd_to_id_.end())
            {
                if (auto t = timers_.find(it->second); t != timers_.end() && t->second.seq > collected_at)
                    continue;
                dispatch_timer(fd);
            }
            else if (auto it = io_handlers_.find(fd); it != io_handlers_.end())
            {
                if (it->second.seq > collected_at)
                    continue;
                dispatch_io(fd, ev);
            }
        }
    }
}

void EventLoop::stop() noexcept
{
    running_ = false;
}

// ── I/O ──────────────────────────────────────────────────────────────────────

// When APOSTOL_EPOLL_ET is enabled, user-facing I/O fds are registered with
// edge-triggered + one-shot semantics: handlers must drain to EAGAIN and
// explicitly re-arm (see rearm_io) or remove themselves. This is a
// defence-in-depth property against forgotten remove_io spins — see the
// plan docs/plans/2026-04-19-epoll-edge-triggered-migration.md.
// Internal fds registered directly via epoll_ctl (timer fds, signal fd)
// continue to use level-triggered — they are owned by EventLoop and
// drained internally.
static constexpr uint32_t kExtraFlags =
#ifdef APOSTOL_EPOLL_ET
    EPOLLET | EPOLLONESHOT
#else
    0
#endif
    ;

void EventLoop::add_io(int fd, uint32_t events, IOCallback cb)
{
    epoll_event ev{};
    ev.events = events | kExtraFlags;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::system_error(errno, std::system_category(),
            fmt::format("epoll_ctl ADD fd={}", fd));

    io_handlers_[fd] = {events, std::move(cb), ++registration_seq_};
}

namespace
{
// Process-wide: every loop of the process reports to the same log. Atomic only
// for the reader's sake; it is set once, before any loop runs.
std::atomic<Logger*> g_diagnostics{nullptr};

// EPOLL_CTL_MOD failed because the socket a handler was registered for is
// gone: EBADF — the descriptor is closed; ENOENT — the registration went with
// a close (the number may be open again, under anyone); EPERM — the number
// now names something epoll cannot watch. One policy for modify_io() and
// rearm_io(): the handler is dropped, nothing is registered in its place.
bool registration_lost(int err) noexcept
{
    return err == EBADF || err == ENOENT || err == EPERM;
}
}

// An exception out of an I/O handler used to leave the loop and end the
// process — a worker with every connection it served, for one bad frame on one
// of them (T637). It is caught here, for this handler only: timers and signals
// still unwind, a process may be counting on that to be restarted.
//
// The loop tells the holder, not the peer. The fd is not closed — it is the
// holder's, and a number closed behind its back is the next socket's (T619) —
// and not shut for writing: the peer would see the end before the holder
// chose it (a leader lock released while its process still drains), and the
// holder's own goodbye (TLS close_notify) could not go out. Its read side
// is shut and the holder gets one more event — end of input, which it takes
// for a lost peer and tears the connection down by its own road, closing the
// fd itself. A holder that throws again, keeps the fd after that event, or
// whose socket cannot be shut (not a socket, not connected) is taken off the
// loop; its fd stays open until the holder closes it.
void EventLoop::dispatch_io(int fd, uint32_t events)
{
    auto it = io_handlers_.find(fd);
    // Copy before calling: callback may remove_io(fd), invalidating 'it'
    auto cb = it->second.callback;
    const uint64_t seq = it->second.seq;
    const bool faulted = it->second.faulted;

    try {
        cb(events);
    } catch (const std::exception& e) {
        io_handler_threw(fd, seq, e.what());
        return;
    } catch (...) {
        io_handler_threw(fd, seq, "unknown exception");
        return;
    }

    if (!faulted)
        return;
    it = io_handlers_.find(fd);
    if (it == io_handlers_.end() || it->second.seq != seq)
        return;   // the holder let go, as it should
    remove_io(fd);
    if (Logger* log = g_diagnostics.load(std::memory_order_relaxed)) {
        try {
            log->error("I/O handler for fd={} kept it after end of input — taken off the loop", fd);
        } catch (...) {}
    }
}

void EventLoop::io_handler_threw(int fd, uint64_t seq, const char* what) noexcept
{
    Logger* log = g_diagnostics.load(std::memory_order_relaxed);
    const char* outcome;
    int err = 0;

    auto it = io_handlers_.find(fd);
    if (it == io_handlers_.end() || it->second.seq != seq) {
        // Removed, or the number is someone else's by now: nothing here to act on.
        outcome = "the handler had let go of it";
    } else if (it->second.faulted) {
        remove_io(fd);
        outcome = "threw again on end of input — taken off the loop";
    } else if (::shutdown(fd, SHUT_RD) < 0) {
        err = errno;
        remove_io(fd);
        outcome = "its socket could not be shut — taken off the loop";
    } else {
        it->second.faulted = true;
        // End of input is EPOLLIN|EPOLLRDHUP: a holder waiting for EPOLLOUT alone
        // would never hear it. MOD also re-arms under ET, where the throw came
        // before the handler's own rearm.
        const uint32_t mask = it->second.events | EPOLLIN | EPOLLRDHUP;
        bool armed = false;
        try { armed = modify_io(fd, mask); } catch (...) {}
        if (armed) {
            outcome = "read side shut, the holder gets end of input";
        } else {
            remove_io(fd);
            outcome = "read side shut, could not be re-armed — taken off the loop";
        }
    }

    if (log) {
        try {
            if (err)
                log->error("I/O handler for fd={} threw: {} — {} ({})", fd, what, outcome,
                           std::system_category().message(err));
            else
                log->error("I/O handler for fd={} threw: {} — {}", fd, what, outcome);
        } catch (...) {}
    }
}

void EventLoop::set_diagnostics(Logger* logger) noexcept
{
    g_diagnostics.store(logger, std::memory_order_relaxed);
}

void EventLoop::unset_diagnostics(Logger* logger) noexcept
{
    g_diagnostics.compare_exchange_strong(logger, nullptr, std::memory_order_relaxed);
}

bool EventLoop::modify_io(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.events = events | kExtraFlags;
    ev.data.fd = fd;

    auto it = io_handlers_.find(fd);

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0)
    {
        if (it != io_handlers_.end())
            it->second.events = events;
        return true;
    }

    const int err = errno;
    if (!registration_lost(err))
        throw std::system_error(err, std::system_category(),
            fmt::format("epoll_ctl MOD fd={}", fd));

    // No handler: the caller let go of this fd itself (remove_io) and is
    // asking about a registration it no longer holds — nothing to report.
    if (it == io_handlers_.end())
        return false;

    // Not registered again even when the number is open: whoever opened it is
    // not known here, and a holder that keeps going on a number it did not
    // open reads a socket that is not its own. The loud path — the holder
    // drops the connection and makes a new one — is the one that cannot go
    // wrong quietly. A library that swaps sockets under its holder is fixed
    // at the source (libpq, T596), not here.
    io_handlers_.erase(it);
    if (Logger* log = g_diagnostics.load(std::memory_order_relaxed))
        log->error("epoll_ctl MOD fd={}: {} — handler dropped, the caller's socket is not watched",
                   fd, std::system_category().message(err));
    return false;
}

void EventLoop::rearm_io(int fd, uint32_t events)
{
#ifdef APOSTOL_EPOLL_ET
    auto it = io_handlers_.find(fd);
    if (it == io_handlers_.end())
        return;   // defensive: handler may have just removed itself

    epoll_event ev{};
    ev.events  = (events ? events : it->second.events) | EPOLLET | EPOLLONESHOT;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        // The socket this fd names may have been closed (EBADF) or already
        // dropped from this epoll instance (ENOENT/EPERM) by a re-entrant
        // teardown that ran *inside* the handler we are rearming after —
        // e.g. a peer half-close, a libpq connection reset/replace, or a
        // WebSocket reconnect that closed this very socket. That is a
        // routine, recoverable condition: there is simply nothing left to
        // rearm. Drop the now-stale handler so a later event on a reused fd
        // number cannot dispatch a dead handler (run() looks handlers up by
        // fd), then return. This must NOT escalate to an exception: a single
        // mistimed fd closure would otherwise unwind the whole worker and
        // tear down every other healthy connection it serves.
        const int err = errno;
        if (registration_lost(err))
        {
            io_handlers_.erase(it);
            return;
        }
        throw std::system_error(err, std::system_category(),
            fmt::format("epoll_ctl MOD (rearm) fd={}", fd));
    }

    if (events)
        it->second.events = events;
#else
    (void)fd;
    (void)events;   // level-triggered: nothing to rearm
#endif
}

void EventLoop::remove_io(int fd)
{
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    io_handlers_.erase(fd);
}

// ── Timers ───────────────────────────────────────────────────────────────────

EventLoop::TimerId EventLoop::add_timer(std::chrono::milliseconds interval, TimerCallback cb, bool repeat)
{
    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        throw std::system_error(errno, std::system_category(), "timerfd_create");

    auto sec = std::chrono::duration_cast<std::chrono::seconds>(interval);
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(interval - sec);

    itimerspec ts{};
    ts.it_value.tv_sec = sec.count();
    ts.it_value.tv_nsec = nsec.count();
    if (repeat)
        ts.it_interval = ts.it_value;

    if (::timerfd_settime(tfd, 0, &ts, nullptr) < 0)
    {
        ::close(tfd);
        throw std::system_error(errno, std::system_category(), "timerfd_settime");
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tfd, &ev) < 0)
    {
        ::close(tfd);
        throw std::system_error(errno, std::system_category(), "epoll_ctl timer ADD");
    }

    TimerId id = next_timer_id_++;
    timers_[id] = {tfd, repeat, std::move(cb), ++registration_seq_};
    timer_fd_to_id_[tfd] = id;
    return id;
}

void EventLoop::cancel_timer(TimerId id)
{
    auto it = timers_.find(id);
    if (it == timers_.end())
        return;

    int tfd = it->second.fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tfd, nullptr);
    ::close(tfd);
    timer_fd_to_id_.erase(tfd);
    timers_.erase(it);
}

void EventLoop::dispatch_timer(int timer_fd)
{
    // Consume the expiration count — required to re-arm EPOLLIN
    // Nothing read, nothing expired: the event was not this timer's (T619) or
    // a nested run_for() already took the expiry. Fired anyway, it went off
    // early, or twice.
    uint64_t count = 0;
    if (::read(timer_fd, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count)))
        return;

    auto id_it = timer_fd_to_id_.find(timer_fd);
    if (id_it == timer_fd_to_id_.end())
        return;

    TimerId id = id_it->second;
    auto it = timers_.find(id);
    if (it == timers_.end())
        return;

    // Copy state before calling (callback may cancel this timer)
    auto cb = it->second.callback;
    bool repeat = it->second.repeat;

    cb();

    if (!repeat && timers_.count(id))
        cancel_timer(id);
}

// ── Signals ──────────────────────────────────────────────────────────────────

void EventLoop::rebuild_signal_fd()
{
    if (signal_fd_ >= 0)
    {
        // Update the existing fd with the new mask
        if (::signalfd(signal_fd_, &signal_mask_, SFD_NONBLOCK | SFD_CLOEXEC) < 0)
            throw std::system_error(errno, std::system_category(), "signalfd update");
    }
    else
    {
        signal_fd_ = ::signalfd(-1, &signal_mask_, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_fd_ < 0)
            throw std::system_error(errno, std::system_category(), "signalfd create");

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = signal_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &ev) < 0)
            throw std::system_error(errno, std::system_category(), "epoll_ctl signalfd ADD");
    }
}

void EventLoop::add_signal(int signum, SignalCallback cb)
{
    // Block the signal so it is delivered to signalfd, not a handler
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    ::sigprocmask(SIG_BLOCK, &mask, nullptr);

    sigaddset(&signal_mask_, signum);
    sig_handlers_[signum] = std::move(cb);
    rebuild_signal_fd();
}

void EventLoop::remove_signal(int signum)
{
    sig_handlers_.erase(signum);
    sigdelset(&signal_mask_, signum);

    // Unblock the signal
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);

    if (!sig_handlers_.empty())
        rebuild_signal_fd();
}

void EventLoop::dispatch_signals()
{
    signalfd_siginfo info{};
    while (::read(signal_fd_, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info)))
    {
        int signum = static_cast<int>(info.ssi_signo);
        if (auto it = sig_handlers_.find(signum); it != sig_handlers_.end())
            it->second(info);
    }
}

} // namespace apostol
