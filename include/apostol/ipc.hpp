#pragma once

// Ipc — messages between the processes of one node.
//
// The master gives every child two socket pairs before it forks it, both
// AF_UNIX/SOCK_SEQPACKET:
//
//   inbox    — end [1] is read by the child alone, on its own event loop; end
//              [0] means "write to this child" and is handed by the master to
//              every other process: inherited by the children forked later,
//              passed by SCM_RIGHTS to those already running. Many processes
//              write into one inbox; a datagram is delivered whole or not at
//              all, so their messages never interleave inside a datagram, and
//              a message longer than one datagram is cut into chunks put back
//              together by (sender slot, generation, id).
//   control  — written by the master alone: "open slot s" with the end [0] of
//              a new inbox, "close slot s" when a process is reaped. Separate
//              from the inbox so that data filling an inbox cannot hold back a
//              change of who is on the node.
//
// The master keeps no copy of end [1]: when a process dies its inbox is gone
// at once, and a write to it fails instead of piling up until the reap.
//
// Everything runs on the process's event loop; nothing here blocks or starts a
// thread. Only within one node: between nodes it is the database. At most one
// delivery, nothing is stored; a process that dies takes its messages with it,
// and whoever waited for it gets an error, not silence.
//
// An application turns it on with Application::enable_ipc() before the master
// forks. Without that — and in a process with no master (single, a stand-alone
// helper) — ipc() still works, alone: a request to itself is answered, every
// other target is unreachable.

#include "apostol/process.hpp"

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apostol
{

class EventLoop;
class Logger;

namespace detail { struct IpcCore; }

enum class IpcError : std::uint8_t
{
    none,
    timeout,      // no answer in time
    gone,         // the process is not there (exited, or never was)
    no_handler,   // the process has nothing registered for the type
    too_large,    // the message is over kMaxMessage
    failed,       // the handler threw, or dropped the reply unanswered
    overflow,     // the queue towards that process is full
    stopping,     // the process is stopping
    unreachable,  // no channel to the process
};

std::string_view ipc_error_name(IpcError e) noexcept;

/// A process of the node, as the directory knows it.
struct IpcPeer
{
    int           slot{-1};        // -1 — no directory (the process is alone)
    ProcessRole   role{ProcessRole::single};
    std::string   name;            // "worker#2", "helper", a custom process's name
    int           number{0};       // within the role: worker#2 → 2; others 0
    pid_t         pid{0};
    std::uint32_t generation{0};
};

struct IpcResult
{
    IpcError    error{IpcError::none};
    std::string body;
    bool ok() const noexcept { return error == IpcError::none; }
};

struct IpcGathered
{
    std::vector<std::pair<IpcPeer, std::string>> replies;
    std::vector<std::pair<IpcPeer, IpcError>>    missing;   // every target not in replies
};

/// Which processes a gather or a broadcast is for.
enum IpcRoles : unsigned
{
    ipc_workers = 1u << 0,
    ipc_helper  = 1u << 1,
    ipc_custom  = 1u << 2,
    ipc_all     = ipc_workers | ipc_helper | ipc_custom,
};

/// The answer to one request — given once, now or later (after a query, say).
/// Dropped unanswered, it answers `failed`. For a message sent without
/// expecting an answer it does nothing.
class IpcReply
{
public:
    IpcReply() = default;
    ~IpcReply();
    IpcReply(IpcReply&& other) noexcept;
    IpcReply& operator=(IpcReply&& other) noexcept;
    IpcReply(const IpcReply&)            = delete;
    IpcReply& operator=(const IpcReply&) = delete;

    /// True until answered, and only for a request.
    bool pending() const noexcept { return active_; }
    void send(std::string body);
    void fail();

private:
    friend struct detail::IpcCore;
    void answer(std::uint8_t status, std::string body);

    std::weak_ptr<detail::IpcCore> core_;
    int           slot_{-1};
    std::uint32_t generation_{0};
    std::uint32_t id_{0};
    std::uint32_t timeout_ms_{0};
    std::string   type_;
    bool          active_{false};
};

class Ipc
{
public:
    using Handler     = std::function<void(const IpcPeer& from, std::string_view body, IpcReply reply)>;
    using RequestDone = std::function<void(IpcResult)>;
    using GatherDone  = std::function<void(IpcGathered)>;

    static constexpr std::size_t kChunk      = 32 * 1024;          // body bytes per datagram
    static constexpr std::size_t kMaxMessage = 16 * 1024 * 1024;
    static constexpr std::size_t kMaxQueue   = 64 * 1024 * 1024;   // per destination
    static constexpr std::size_t kMaxType    = 64;

    Ipc();
    ~Ipc();
    Ipc(const Ipc&)            = delete;
    Ipc& operator=(const Ipc&) = delete;

    /// Channels to the other processes of the node exist.
    bool connected() const noexcept;

    /// The process is stopping: its channels are being closed, or are closed.
    /// True from the moment the stop begins — so a callback of request(),
    /// gather() or Application::collect_metrics() that the stop ends sees it
    /// true, and can tell "this process is stopping" from "the others did not
    /// answer". A request or a gather started from then on is refused as
    /// `stopping`.
    bool stopping() const noexcept;

    IpcPeer self() const;
    /// Live processes of the node in @p roles, this one included — the targets
    /// of a gather. Retiring ones (a reload, a shutdown) are left out.
    std::vector<IpcPeer> peers(unsigned roles = ipc_all) const;

    /// One handler per type; a second registration replaces the first.
    void on(std::string type, Handler handler);
    void off(std::string_view type);

    /// No answer expected. False — refused at once (too large, overflow,
    /// unreachable); true says it was queued, not that it arrived.
    bool send(int slot, std::string_view type, std::string body);

    /// Exactly one call of @p done: the answer, or an error.
    void request(int slot, std::string_view type, std::string body,
                 RequestDone done, std::chrono::milliseconds timeout);

    /// Ask every live process in @p roles, this one included. @p done is
    /// called once, when all have answered or @p timeout has passed; every
    /// target is either in `replies` or in `missing`, none is lost.
    void gather(unsigned roles, std::string_view type, std::string body,
                GatherDone done, std::chrono::milliseconds timeout);

    /// send() to every live process in @p roles but this one.
    void broadcast(unsigned roles, std::string_view type, std::string body);

    /// Counters for a metrics endpoint. `type` is a registered type or "other".
    struct Stats
    {
        // (type, result) → messages handled here: result "ok" or an IpcError name
        std::map<std::pair<std::string, std::string>, std::uint64_t> received;
        // (type, result) → requests sent from here, by how they ended
        std::map<std::pair<std::string, std::string>, std::uint64_t> requests;
        // (type, reason) → targets of a gather from here that did not answer
        std::map<std::pair<std::string, std::string>, std::uint64_t> gather_missing;
        std::uint64_t dropped{0};   // datagrams that could not be read or put together
        std::uint64_t blocked{0};   // writes that found a channel full and waited for room
        std::uint64_t held{0};      // messages kept until the channel to their process opened
    };
    const Stats& stats() const noexcept;

private:
    friend class Application;

    // ── Application: the master ──────────────────────────────────────────────
    void enable() noexcept;
    bool enabled() const noexcept;
    bool master_create(Logger& log);
    /// Before fork: a slot and the two pairs. False — the child runs alone.
    bool master_prepare(ProcessRole role, std::string_view name, int number);
    void master_forked_child();          // in the child, right after fork
    void master_forked_parent(pid_t pid);
    void master_fork_failed();
    void master_retire(pid_t pid);
    void master_reaped(pid_t pid);
    void master_attach(EventLoop& loop);
    void master_detach();

    // ── Application: every process that runs a loop ──────────────────────────
    void attach(EventLoop& loop, Logger& log, ProcessRole role, std::string_view name);
    void ready();     // started: live in the directory
    void detach();    // stopping: pending requests fail, handlers are dropped

    std::shared_ptr<detail::IpcCore> core_;
};

} // namespace apostol
