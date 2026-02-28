#include "apostol/event_loop.hpp"

#include <cerrno>
#include <fmt/format.h>
#include <stdexcept>
#include <system_error>

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
    epoll_event events[MAX_EVENTS];

    while (running_)
    {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::system_category(), "epoll_wait");
        }

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
                dispatch_timer(fd);
            }
            else if (auto it = io_handlers_.find(fd); it != io_handlers_.end())
            {
                // Copy before calling: callback may remove_io(fd), invalidating 'it'
                auto cb = it->second.callback;
                cb(ev);
            }
        }
    }
}

void EventLoop::stop() noexcept
{
    running_ = false;
}

// ── I/O ──────────────────────────────────────────────────────────────────────

void EventLoop::add_io(int fd, uint32_t events, IOCallback cb)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::system_error(errno, std::system_category(),
            fmt::format("epoll_ctl ADD fd={}", fd));

    io_handlers_[fd] = {events, std::move(cb)};
}

void EventLoop::modify_io(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        throw std::system_error(errno, std::system_category(),
            fmt::format("epoll_ctl MOD fd={}", fd));

    if (auto it = io_handlers_.find(fd); it != io_handlers_.end())
        it->second.events = events;
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
    timers_[id] = {tfd, repeat, std::move(cb)};
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
    uint64_t count = 0;
    ::read(timer_fd, &count, sizeof(count));

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
