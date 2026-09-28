#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

namespace apostol
{

class Logger;

// ─── EventLoop ───────────────────────────────────────────────────────────────
//
// Single-threaded event loop based on epoll + timerfd + signalfd.
//
// Usage:
//   EventLoop loop;
//   loop.add_signal(SIGTERM, [&loop](auto&) { loop.stop(); });
//   loop.add_timer(500ms, [] { /* tick */ });
//   loop.run();   // blocks until stop() is called
//
class EventLoop
{
public:
    using IOCallback = std::function<void(uint32_t events)>;
    using TimerCallback = std::function<void()>;
    using SignalCallback = std::function<void(const signalfd_siginfo&)>;
    using TimerId = int;

    static constexpr TimerId kInvalidTimer = -1;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Run the event loop until stop() is called.
    void run();

    /// Run the loop until @p duration elapses, or until stop() is called, or until
    /// @p done returns true — whichever comes first. Returns true if @p done was
    /// satisfied.
    ///
    /// For draining at shutdown. run() returns when the process is stopping, but
    /// work queued after that — a session to close, a last write — would otherwise
    /// sit in a queue nobody pumps again.
    bool run_for(std::chrono::milliseconds duration,
                 const std::function<bool()>& done = {});

    // Request loop termination (safe to call from within a callback).
    void stop() noexcept;

    bool running() const noexcept { return running_; }

    // ── I/O ──────────────────────────────────────────────────────────────────

    // Register fd for epoll. events: EPOLLIN, EPOLLOUT, EPOLLET, etc.
    void add_io(int fd, uint32_t events, IOCallback cb);

    // Change monitored events for an already-registered fd. Returns true when
    // the fd is registered with @p events afterwards.
    //
    // A registration can vanish under its holder: epoll drops it when the
    // descriptor is closed, whoever closed it. That used to throw out of the
    // loop and take the process down (T596, T607). Now, on EBADF, ENOENT or
    // EPERM the handler is dropped — a later socket with this number cannot
    // reach it — an error is logged through set_diagnostics(), and false is
    // returned: the caller's socket is not watched any more, and the caller
    // must treat it as a lost connection (close it, fail what was in flight,
    // reconnect). The fd is never registered again here, even when the number
    // is open: this loop cannot tell who opened it. Any other error throws.
    [[nodiscard]] bool modify_io(int fd, uint32_t events);

    // Where EventLoop reports what it tolerated instead of throwing. One per
    // process, for every loop in it; Application sets its log here.
    // nullptr (the default) — not reported.
    static void set_diagnostics(Logger* logger) noexcept;
    // Clears the setting only if @p logger is the one set.
    static void unset_diagnostics(Logger* logger) noexcept;

    // Re-arm a fd registered with APOSTOL_EPOLL_ET (edge-triggered + one-shot)
    // so further events resume being delivered. With @p events==0 (default)
    // the mask last set by add_io or modify_io is reused. No-op if the fd is
    // not currently registered (defensive — a handler may have removed it
    // just before rearm was requested). With APOSTOL_EPOLL_ET disabled, this
    // is a cheap no-op: level-triggered fds stay armed.
    void rearm_io(int fd, uint32_t events = 0);

    // Remove fd from epoll. Does NOT close it.
    void remove_io(int fd);

    // ── Timers ────────────────────────────────────────────────────────────────

    // Add a timer. Returns a TimerId that can be used to cancel it.
    TimerId add_timer(std::chrono::milliseconds interval, TimerCallback cb, bool repeat = true);

    // Cancel and destroy a timer.
    void cancel_timer(TimerId id);

    // ── Signals ───────────────────────────────────────────────────────────────

    // Register a signal handler via signalfd. The signal is blocked in the
    // process so it can be received through the event loop.
    void add_signal(int signum, SignalCallback cb);

    // Unregister a signal handler and unblock the signal.
    void remove_signal(int signum);

private:
    void rebuild_signal_fd();
    void dispatch_signals();
    void dispatch_timer(int timer_fd);

    int epoll_fd_{-1};
    int signal_fd_{-1};
    /// One epoll_wait plus dispatch. @p timeout_ms is passed through: -1 blocks.
    void poll_once(int timeout_ms);

    bool running_{false};

    struct IOEntry
    {
        uint32_t events;
        IOCallback callback;
    };

    struct TimerEntry
    {
        int fd;
        bool repeat;
        TimerCallback callback;
    };

    std::unordered_map<int, IOEntry> io_handlers_;       // fd → entry
    std::unordered_map<TimerId, TimerEntry> timers_;     // id → entry
    std::unordered_map<int, TimerId> timer_fd_to_id_;    // timer_fd → id
    std::unordered_map<int, SignalCallback> sig_handlers_; // signum → cb

    sigset_t signal_mask_{};
    TimerId next_timer_id_{1};

    static constexpr int MAX_EVENTS = 512;
};

} // namespace apostol
