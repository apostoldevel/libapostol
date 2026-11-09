#include "apostol/ipc.hpp"

#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/node_directory.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <compare>
#include <cstring>
#include <deque>
#include <exception>
#include <unordered_map>

namespace apostol
{

namespace
{

constexpr std::uint32_t kFrameMagic   = 0x31435041;   // "APC1"
constexpr std::uint32_t kControlMagic = 0x43435041;   // "APCC"
constexpr long long     kHoldMs       = 5000;         // a reply or a send waiting for "open"
constexpr long long     kPartMs       = 60000;        // a message not put together by then is dropped

enum Kind : std::uint8_t { k_oneway = 1, k_request = 2, k_reply = 3 };
enum Flag : std::uint8_t { f_first = 1, f_last = 2 };
enum Op   : std::uint8_t { op_open = 1, op_close = 2 };

// Host byte order: a frame never leaves the host.
struct FrameHeader
{
    std::uint32_t magic;
    std::uint8_t  kind;
    std::uint8_t  status;       // reply: an IpcError, none = ok
    std::uint8_t  flags;
    std::uint8_t  type_len;     // the type follows the header in the first chunk
    std::uint16_t from_slot;
    std::uint16_t reserved;
    std::uint32_t from_generation;
    std::uint32_t id;           // a reply carries the id of its request
    std::uint32_t total;        // body bytes of the whole message
    std::uint32_t offset;       // of this chunk's body bytes
    std::uint32_t timeout_ms;   // request: how long its sender waits
};
static_assert(sizeof(FrameHeader) == 32);

struct ControlFrame
{
    std::uint32_t magic;
    std::uint8_t  op;
    std::uint8_t  pad;
    std::uint16_t slot;
    std::uint32_t generation;
};
static_assert(sizeof(ControlFrame) == 12);

void close_fd(int& fd) noexcept
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

bool make_pair(int fds[2]) noexcept
{
    return ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0;
}

std::vector<std::string> make_frames(std::uint8_t kind, std::uint8_t status, int from_slot,
                                     std::uint32_t from_generation, std::uint32_t id,
                                     std::uint32_t timeout_ms, std::string_view type,
                                     std::string_view body)
{
    std::vector<std::string> frames;
    std::size_t offset = 0;
    do
    {
        const bool first = offset == 0;
        const std::size_t n = std::min(Ipc::kChunk, body.size() - offset);
        const bool last = offset + n == body.size();

        FrameHeader h{};
        h.magic           = kFrameMagic;
        h.kind            = kind;
        h.status          = status;
        h.flags           = static_cast<std::uint8_t>((first ? f_first : 0) | (last ? f_last : 0));
        h.type_len        = first ? static_cast<std::uint8_t>(type.size()) : 0;
        h.from_slot       = static_cast<std::uint16_t>(from_slot);
        h.from_generation = from_generation;
        h.id              = id;
        h.total           = static_cast<std::uint32_t>(body.size());
        h.offset          = static_cast<std::uint32_t>(offset);
        h.timeout_ms      = timeout_ms;

        std::string d;
        d.reserve(sizeof(h) + h.type_len + n);
        d.append(reinterpret_cast<const char*>(&h), sizeof(h));
        if (first)
            d.append(type);
        d.append(body.substr(offset, n));
        frames.push_back(std::move(d));
        offset += n;
    } while (offset < body.size());
    return frames;
}

bool role_in(ProcessRole role, unsigned roles) noexcept
{
    switch (role)
    {
        case ProcessRole::worker:
        case ProcessRole::single: return (roles & ipc_workers) != 0;
        case ProcessRole::helper: return (roles & ipc_helper) != 0;
        case ProcessRole::custom: return (roles & ipc_custom) != 0;
        default:                  return false;
    }
}

long long now_ms() noexcept { return NodeDirectory::now_ms(); }

} // namespace

std::string_view ipc_error_name(IpcError e) noexcept
{
    switch (e)
    {
        case IpcError::none:        return "ok";
        case IpcError::timeout:     return "timeout";
        case IpcError::gone:        return "gone";
        case IpcError::no_handler:  return "no_handler";
        case IpcError::too_large:   return "too_large";
        case IpcError::failed:      return "failed";
        case IpcError::overflow:    return "overflow";
        case IpcError::stopping:    return "stopping";
        case IpcError::unreachable: return "unreachable";
    }
    return "failed";
}

// ─── IpcCore ─────────────────────────────────────────────────────────────────

struct detail::IpcCore : std::enable_shared_from_this<detail::IpcCore>
{
    bool           enabled{false};
    NodeDirectory  dir;
    Logger*        log{nullptr};
    EventLoop*     loop{nullptr};
    Ipc::Stats     stats;

    // ── master ───────────────────────────────────────────────────────────────
    struct ControlMsg
    {
        ControlFrame frame;
        int          fd;        // to pass, or -1; owned by the child entry it belongs to
    };
    struct Child
    {
        pid_t                  pid{0};
        int                    slot{-1};
        std::uint32_t          generation{0};
        int                    inbox_w{-1};    // end [0] of its inbox
        int                    control{-1};    // the master's end of its control pair
        std::deque<ControlMsg> out;
        bool                   watching{false};
        long long              forked_ms{0};
        bool                   warned{false};
    };
    struct Prepared
    {
        bool          valid{false};
        int           slot{-1};
        std::uint32_t generation{0};
        int           inbox[2]{-1, -1};
        int           control[2]{-1, -1};
        ProcessRole   role{ProcessRole::worker};
        int           number{0};
        std::string   name;
    };
    bool               is_master{false};
    std::vector<Child> children;
    Prepared           prep;
    std::uint32_t      next_generation{1};
    EventLoop::TimerId master_timer{EventLoop::kInvalidTimer};

    // ── a process of the node ────────────────────────────────────────────────
    struct Link
    {
        std::uint32_t           generation{0};
        int                     fd{-1};
        std::deque<std::string> out;
        std::size_t             bytes{0};
        bool                    watching{false};
    };
    struct Held                 // for a process whose channel is not open here yet
    {
        std::uint32_t            generation{0};
        std::vector<std::string> frames;
        std::size_t              bytes{0};
        long long                deadline_ms{0};
        std::uint32_t            request_id{0};
    };
    struct Pending
    {
        int                slot{-1};
        std::uint32_t      generation{0};
        std::string        type;
        EventLoop::TimerId timer{EventLoop::kInvalidTimer};
        Ipc::RequestDone   done;
    };
    struct PartKey
    {
        int           slot;
        std::uint32_t generation;
        std::uint32_t id;
        std::uint8_t  kind;
        auto operator<=>(const PartKey&) const = default;
    };
    struct Part
    {
        std::string   type;
        std::uint8_t  status{0};
        std::uint32_t timeout_ms{0};
        std::uint32_t total{0};
        std::string   body;
        long long     started_ms{0};
    };

    bool    connected{false};   // forked by a master that gave it channels
    bool    attached{false};
    bool    stopping{false};
    IpcPeer me;
    int     inbox_fd{-1};
    int     control_fd{-1};

    std::unordered_map<int, Link>              links;
    std::unordered_map<int, std::vector<Held>> held;
    std::unordered_map<std::uint32_t, Pending> pending;
    std::map<PartKey, Part>                    parts;
    std::unordered_map<std::string, Ipc::Handler> handlers;
    std::uint32_t                              next_id{1};
    std::deque<std::function<void()>>          deferred;
    EventLoop::TimerId                         defer_timer{EventLoop::kInvalidTimer};
    EventLoop::TimerId                         beat_timer{EventLoop::kInvalidTimer};
    std::vector<char>                          rbuf;

    // ── helpers ──────────────────────────────────────────────────────────────

    template <typename... Args>
    void warn(fmt::format_string<Args...> f, Args&&... args)
    {
        if (log)
            try { log->warn(f, std::forward<Args>(args)...); } catch (...) {}
    }

    std::uint32_t new_id() noexcept
    {
        if (next_id == 0)
            next_id = 1;
        return next_id++;
    }

    static void bump(std::map<std::pair<std::string, std::string>, std::uint64_t>& m,
                     std::string_view type, std::string_view result)
    {
        ++m[{std::string(type), std::string(result)}];
    }

    std::string_view label(std::string_view type) const
    {
        return handlers.count(std::string(type)) ? type : std::string_view("other");
    }

    IpcPeer peer_of(int slot, std::uint32_t generation) const
    {
        if (slot == me.slot && generation == me.generation)
            return me;
        IpcPeer p;
        p.slot       = slot;
        p.generation = generation;
        if (auto e = dir.get(slot); e && e->generation == generation)
        {
            p.role   = e->role;
            p.name   = e->name;
            p.number = e->number;
            p.pid    = e->pid;
        }
        return p;
    }

    bool is_self(int slot, std::uint32_t generation) const noexcept
    {
        return slot == me.slot && generation == me.generation;
    }

    // ── deferred work: callbacks never run inside the call that caused them ───

    void defer(std::function<void()> fn)
    {
        deferred.push_back(std::move(fn));
        if (!loop || defer_timer != EventLoop::kInvalidTimer)
            return;
        std::weak_ptr<IpcCore> w = weak_from_this();
        defer_timer = loop->add_timer(std::chrono::milliseconds(1), [w] {
            if (auto c = w.lock())
            {
                c->defer_timer = EventLoop::kInvalidTimer;
                c->run_deferred();
            }
        }, /*repeat=*/false);
    }

    void run_deferred()
    {
        auto batch = std::move(deferred);
        deferred.clear();
        for (auto& fn : batch)
        {
            try { fn(); }
            catch (const std::exception& e) { warn("ipc: callback failed: {}", e.what()); }
            catch (...) { warn("ipc: callback failed"); }
        }
    }

    // ── requests ─────────────────────────────────────────────────────────────

    // The request ends now: no reply or timeout can reach it any more. @p later —
    // its callback runs from the loop, not here: the caller may be inside
    // request(), send() or a reply, which must not call back into their caller.
    void complete(std::uint32_t id, IpcResult result, bool later = false)
    {
        auto it = pending.find(id);
        if (it == pending.end())
            return;
        Pending p = std::move(it->second);
        pending.erase(it);
        if (p.timer != EventLoop::kInvalidTimer && loop)
            loop->cancel_timer(p.timer);
        drop_held_request(p.slot, id);
        bump(stats.requests, p.type, ipc_error_name(result.error));
        if (!p.done)
            return;
        if (later)
        {
            defer([done = std::move(p.done), result = std::move(result)]() mutable {
                done(std::move(result));
            });
            return;
        }
        try { p.done(std::move(result)); }
        catch (const std::exception& e) { warn("ipc: request callback failed: {}", e.what()); }
        catch (...) { warn("ipc: request callback failed"); }
    }

    // A request still held for "open" when it times out was never sent: the
    // process was not reachable, not slow.
    bool drop_held_request(int slot, std::uint32_t id)
    {
        auto it = held.find(slot);
        if (it == held.end())
            return false;
        auto& v = it->second;
        const auto before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [id](const Held& h) { return h.request_id == id; }),
                v.end());
        const bool found = v.size() != before;
        if (v.empty())
            held.erase(it);
        return found;
    }

    void on_timeout(std::uint32_t id)
    {
        auto it = pending.find(id);
        if (it == pending.end())
            return;
        it->second.timer = EventLoop::kInvalidTimer;   // one-shot: already gone
        const bool was_held = drop_held_request(it->second.slot, id);
        complete(id, {was_held ? IpcError::unreachable : IpcError::timeout, {}});
    }

    void fail_pending(int slot, std::uint32_t generation, IpcError error)
    {
        std::vector<std::uint32_t> ids;
        for (const auto& [id, p] : pending)
            if (p.slot == slot && p.generation == generation)
                ids.push_back(id);
        for (auto id : ids)
            complete(id, {error, {}}, /*later=*/true);
    }

    // ── sending ──────────────────────────────────────────────────────────────

    IpcError enqueue(int slot, std::uint32_t generation, std::vector<std::string>&& frames,
                     long long hold_ms, std::uint32_t request_id)
    {
        std::size_t bytes = 0;
        for (const auto& f : frames)
            bytes += f.size();

        if (auto it = links.find(slot); it != links.end())
        {
            Link& l = it->second;
            if (l.generation == generation)
            {
                if (l.bytes + bytes > Ipc::kMaxQueue)
                    return IpcError::overflow;
                for (auto& f : frames)
                    l.out.push_back(std::move(f));
                l.bytes += bytes;
                return flush(slot) ? IpcError::none : IpcError::gone;
            }
            if (l.generation > generation)
                return IpcError::gone;
        }

        // No channel to this generation here yet. The directory tells whether it
        // is coming (the master's "open" is on its way) or will never come.
        auto e = dir.get(slot);
        if (!e || e->generation != generation)
            return IpcError::gone;

        auto& v = held[slot];
        std::size_t held_bytes = 0;
        for (const auto& h : v)
            held_bytes += h.bytes;
        if (held_bytes + bytes > Ipc::kMaxQueue)
            return IpcError::overflow;
        v.push_back({generation, std::move(frames), bytes, now_ms() + hold_ms, request_id});
        ++stats.held;
        return IpcError::none;
    }

    void watch_writable(int slot, Link& l)
    {
        if (!loop)
            return;
        if (l.watching)
        {
            loop->rearm_io(l.fd);
            return;
        }
        std::weak_ptr<IpcCore> w = weak_from_this();
        const auto generation = l.generation;
        loop->add_io(l.fd, EPOLLOUT, [w, slot, generation](std::uint32_t) {
            auto c = w.lock();
            if (!c)
                return;
            auto it = c->links.find(slot);
            if (it != c->links.end() && it->second.generation == generation)
                c->flush(slot);
        });
        l.watching = true;
    }

    // False — the channel is dead (the process is gone).
    bool flush(int slot)
    {
        auto it = links.find(slot);
        if (it == links.end())
            return false;
        Link& l = it->second;
        while (!l.out.empty())
        {
            const std::string& d = l.out.front();
            const ssize_t n = ::send(l.fd, d.data(), d.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0)
            {
                l.bytes -= d.size();
                l.out.pop_front();
                continue;
            }
            const int err = errno;
            if (err == EINTR)
                continue;
            if (err == EAGAIN || err == EWOULDBLOCK)
            {
                ++stats.blocked;
                watch_writable(slot, l);
                return true;
            }
            if (err == ENOBUFS || err == ENOMEM)
            {
                // Short of kernel memory, not of room in the channel: no
                // EPOLLOUT will tell when it passes. The queue stays; on_beat
                // tries again.
                ++stats.blocked;
                if (l.watching && loop)
                {
                    loop->remove_io(l.fd);
                    l.watching = false;
                }
                return true;
            }
            // EPIPE and the like: the process is gone. Anything else is not
            // expected of a local socket — said, then treated the same way.
            if (err != EPIPE && err != ECONNRESET && err != ECONNREFUSED)
            {
                const auto peer = peer_of(slot, l.generation);
                warn("ipc: writing to '{}' (slot {}) failed: {} — the channel is closed",
                     peer.name, slot, std::strerror(err));
            }
            link_dead(slot, l.generation);
            return false;
        }
        if (l.watching && loop)
        {
            loop->remove_io(l.fd);
            l.watching = false;
        }
        return true;
    }

    void link_dead(int slot, std::uint32_t generation)
    {
        auto it = links.find(slot);
        if (it != links.end() && it->second.generation == generation)
        {
            if (it->second.watching && loop)
                loop->remove_io(it->second.fd);
            close_fd(it->second.fd);
            links.erase(it);
        }
        forget(slot, generation);
    }

    // Everything owed to or expected from (slot, generation) ends: it is gone.
    // Reached from inside a send — the callbacks of what ends run later.
    void forget(int slot, std::uint32_t generation)
    {
        if (auto h = held.find(slot); h != held.end())
        {
            std::vector<std::uint32_t> ids;
            auto& v = h->second;
            for (auto i = v.begin(); i != v.end();)
            {
                if (i->generation <= generation)
                {
                    if (i->request_id)
                        ids.push_back(i->request_id);
                    i = v.erase(i);
                }
                else
                    ++i;
            }
            if (v.empty())
                held.erase(h);
            for (auto id : ids)
                complete(id, {IpcError::gone, {}}, /*later=*/true);
        }
        for (auto p = parts.begin(); p != parts.end();)
        {
            if (p->first.slot == slot && p->first.generation <= generation)
                p = parts.erase(p);
            else
                ++p;
        }
        fail_pending(slot, generation, IpcError::gone);
    }

    // ── answering ────────────────────────────────────────────────────────────

    void reply(int slot, std::uint32_t generation, std::uint32_t id, std::uint32_t timeout_ms,
               std::string_view type, std::uint8_t status, std::string body)
    {
        if (body.size() > Ipc::kMaxMessage)
        {
            status = static_cast<std::uint8_t>(IpcError::too_large);
            body.clear();
        }
        bump(stats.received, label(type), ipc_error_name(static_cast<IpcError>(status)));
        // Detached (stopped; the master, which never attaches, still answers
        // itself): no channel is left to carry it, and nothing would ever send
        // it on. The asker gets its timeout, or "gone" when this process exits.
        if (stopping && !attached)
            return;

        if (is_self(slot, generation))
        {
            std::weak_ptr<IpcCore> w = weak_from_this();
            defer([w, id, status, body = std::move(body)]() mutable {
                if (auto c = w.lock())
                    c->deliver_reply(id, c->me.slot, c->me.generation, status, std::move(body));
            });
            return;
        }
        if (!connected)
            return;
        auto frames = make_frames(k_reply, status, me.slot, me.generation, id, 0, {}, body);
        const long long hold = timeout_ms ? static_cast<long long>(timeout_ms) : kHoldMs;
        enqueue(slot, generation, std::move(frames), hold, 0);
    }

    void deliver_reply(std::uint32_t id, int from_slot, std::uint32_t from_generation,
                       std::uint8_t status, std::string body)
    {
        auto it = pending.find(id);
        if (it == pending.end()
            || it->second.slot != from_slot || it->second.generation != from_generation)
            return;   // late, or not ours
        complete(id, {static_cast<IpcError>(status), std::move(body)});
    }

    void deliver_message(const IpcPeer& from, std::uint8_t kind, std::uint32_t id,
                         std::uint32_t timeout_ms, const std::string& type, std::string body)
    {
        const bool is_request = kind == k_request;
        auto answer_now = [&](IpcError e) {
            if (is_request)
                reply(from.slot, from.generation, id, timeout_ms, type, static_cast<std::uint8_t>(e), {});
            else
                bump(stats.received, label(type), ipc_error_name(e));
        };

        if (stopping)
            return answer_now(IpcError::stopping);
        auto h = handlers.find(type);
        if (h == handlers.end())
            return answer_now(IpcError::no_handler);

        IpcReply r;
        if (is_request)
        {
            r.core_       = weak_from_this();
            r.slot_       = from.slot;
            r.generation_ = from.generation;
            r.id_         = id;
            r.timeout_ms_ = timeout_ms;
            r.type_       = type;
            r.active_     = true;
        }
        else
            bump(stats.received, type, "ok");

        auto handler = h->second;   // it may unregister itself
        try
        {
            handler(from, body, std::move(r));
        }
        catch (const std::exception& e)
        {
            warn("ipc: handler '{}' failed: {}", type, e.what());
        }
        catch (...)
        {
            warn("ipc: handler '{}' failed", type);
        }
        // A reply the handler neither kept nor answered answers "failed" as it
        // goes out of scope — here, or wherever the handler moved it.
    }

    // ── receiving ────────────────────────────────────────────────────────────

    void on_datagram(const char* data, std::size_t n)
    {
        FrameHeader h;
        if (n < sizeof(h))
            return void(++stats.dropped);
        std::memcpy(&h, data, sizeof(h));
        const bool first = (h.flags & f_first) != 0;
        const bool last  = (h.flags & f_last) != 0;
        const std::size_t type_len = first ? h.type_len : 0;
        if (h.magic != kFrameMagic || type_len > Ipc::kMaxType || n < sizeof(h) + type_len
            || h.total > Ipc::kMaxMessage
            || (h.kind != k_oneway && h.kind != k_request && h.kind != k_reply))
            return void(++stats.dropped);
        const std::size_t payload = n - sizeof(h) - type_len;
        if (static_cast<std::size_t>(h.offset) + payload > h.total)
            return void(++stats.dropped);

        const char* type_ptr = data + sizeof(h);
        const char* body_ptr = type_ptr + type_len;
        const int   slot     = h.from_slot;

        if (first && last)
        {
            if (h.offset != 0 || payload != h.total)
                return void(++stats.dropped);
            dispatch(slot, h, std::string(type_ptr, type_len), std::string(body_ptr, payload));
            return;
        }

        const PartKey key{slot, h.from_generation, h.id, h.kind};
        if (first)
        {
            if (h.offset != 0)
                return void(++stats.dropped);
            parts.erase(key);
            // What is being put together is bounded like a queue: by the
            // totals announced, not by what has arrived so far.
            std::size_t announced = h.total;
            for (const auto& [k, q] : parts)
                announced += q.total;
            if (announced > Ipc::kMaxQueue)
                return void(++stats.dropped);
            Part& p = parts[key];
            p.type       = std::string(type_ptr, type_len);
            p.status     = h.status;
            p.timeout_ms = h.timeout_ms;
            p.total      = h.total;
            p.started_ms = now_ms();
            p.body.append(body_ptr, payload);
            return;
        }
        auto it = parts.find(key);
        if (it == parts.end() || it->second.body.size() != h.offset || it->second.total != h.total)
        {
            if (it != parts.end())
                parts.erase(it);
            return void(++stats.dropped);
        }
        it->second.body.append(body_ptr, payload);
        if (!last)
            return;
        Part p = std::move(it->second);
        parts.erase(it);
        if (p.body.size() != p.total)
            return void(++stats.dropped);
        h.status     = p.status;
        h.timeout_ms = p.timeout_ms;
        dispatch(slot, h, std::move(p.type), std::move(p.body));
    }

    void dispatch(int slot, const FrameHeader& h, std::string type, std::string body)
    {
        if (h.kind == k_reply)
            return deliver_reply(h.id, slot, h.from_generation, h.status, std::move(body));
        deliver_message(peer_of(slot, h.from_generation), h.kind, h.id, h.timeout_ms,
                        type, std::move(body));
    }

    void on_inbox()
    {
        auto self = shared_from_this();   // a handler may detach us
        if (rbuf.empty())
            rbuf.resize(sizeof(FrameHeader) + Ipc::kMaxType + Ipc::kChunk + 64);
        while (attached && inbox_fd >= 0)
        {
            iovec iov{rbuf.data(), rbuf.size()};
            msghdr msg{};
            msg.msg_iov    = &iov;
            msg.msg_iovlen = 1;
            const ssize_t n = ::recvmsg(inbox_fd, &msg, MSG_DONTWAIT);
            if (n > 0)
            {
                if (msg.msg_flags & MSG_TRUNC)
                {
                    ++stats.dropped;
                    continue;
                }
                on_datagram(rbuf.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (loop)
                    loop->rearm_io(inbox_fd);
                return;
            }
            // 0 — no writer left at all (the master is gone); an error: stop
            // watching, or the loop would spin on it.
            warn("ipc: the inbox of '{}' is closed ({}) — no message reaches this process any more",
                 me.name, n == 0 ? "no writer left" : std::strerror(errno));
            if (loop)
                loop->remove_io(inbox_fd);
            close_fd(inbox_fd);
            return;
        }
    }

    void on_control()
    {
        auto self = shared_from_this();
        while (attached && control_fd >= 0)
        {
            ControlFrame f{};
            iovec iov{&f, sizeof(f)};
            alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))];
            msghdr msg{};
            msg.msg_iov        = &iov;
            msg.msg_iovlen     = 1;
            msg.msg_control    = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            const ssize_t n = ::recvmsg(control_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (loop)
                    loop->rearm_io(control_fd);
                return;
            }
            if (n <= 0)
            {
                warn("ipc: the control channel of '{}' is closed ({}) — no process of the node "
                     "is opened or closed here any more", me.name,
                     n == 0 ? "the master is gone" : std::strerror(errno));
                if (loop)
                    loop->remove_io(control_fd);
                close_fd(control_fd);
                return;
            }

            int fd = -1;
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS
                    && c->cmsg_len >= CMSG_LEN(sizeof(int)))
                    std::memcpy(&fd, CMSG_DATA(c), sizeof(int));

            if (static_cast<std::size_t>(n) != sizeof(f) || f.magic != kControlMagic)
            {
                close_fd(fd);
                ++stats.dropped;
                continue;
            }
            if (f.op == op_open)
                open_link(f.slot, f.generation, fd);
            else
            {
                close_fd(fd);
                if (f.op == op_close)
                    link_dead(f.slot, f.generation);
            }
        }
    }

    void open_link(int slot, std::uint32_t generation, int fd)
    {
        if (fd < 0)
            return;
        if (slot == me.slot)
        {
            int tmp = fd;
            close_fd(tmp);
            return;
        }
        if (auto it = links.find(slot); it != links.end())
        {
            if (it->second.generation >= generation)
            {
                int tmp = fd;
                close_fd(tmp);   // stale
                return;
            }
            link_dead(slot, it->second.generation);   // its predecessor in the slot
        }
        // A predecessor that had no link here still has its waiters.
        forget(slot, generation - 1);

        Link& l = links[slot];
        l.generation = generation;
        l.fd = fd;

        // What waited for this "open" goes now, in the order it was queued.
        if (auto h = held.find(slot); h != held.end())
        {
            auto v = std::move(h->second);
            held.erase(h);
            for (auto& item : v)
            {
                if (item.generation != generation)
                {
                    held[slot].push_back(std::move(item));
                    continue;
                }
                if (item.request_id && !pending.count(item.request_id))
                    continue;   // its sender stopped waiting
                for (auto& d : item.frames)
                    l.out.push_back(std::move(d));
                l.bytes += item.bytes;
            }
        }
        flush(slot);
    }

    void on_beat()
    {
        if (connected)
            dir.stamp(me.slot);
        const long long now = now_ms();
        std::vector<std::uint32_t> ids;
        for (auto h = held.begin(); h != held.end();)
        {
            auto& v = h->second;
            for (auto i = v.begin(); i != v.end();)
            {
                if (i->deadline_ms <= now)
                {
                    if (i->request_id)
                        ids.push_back(i->request_id);
                    i = v.erase(i);
                }
                else
                    ++i;
            }
            h = v.empty() ? held.erase(h) : std::next(h);
        }
        for (auto id : ids)
            complete(id, {IpcError::unreachable, {}});
        // Queues left by a write short of kernel memory (see flush).
        std::vector<int> stuck;
        for (const auto& [slot, l] : links)
            if (!l.out.empty() && !l.watching)
                stuck.push_back(slot);
        for (int slot : stuck)
            flush(slot);
        for (auto p = parts.begin(); p != parts.end();)
        {
            if (now - p->second.started_ms > kPartMs)
            {
                ++stats.dropped;
                p = parts.erase(p);
            }
            else
                ++p;
        }
    }

    // ── master ───────────────────────────────────────────────────────────────

    Child* child_by_pid(pid_t pid)
    {
        for (auto& c : children)
            if (c.pid == pid)
                return &c;
        return nullptr;
    }

    void watch_control(Child& c)
    {
        if (!loop)
            return;
        if (c.watching)
        {
            loop->rearm_io(c.control);
            return;
        }
        std::weak_ptr<IpcCore> w = weak_from_this();
        const pid_t pid = c.pid;
        loop->add_io(c.control, EPOLLOUT, [w, pid](std::uint32_t) {
            if (auto core = w.lock())
                if (auto* ch = core->child_by_pid(pid))
                    core->flush_child(*ch);
        });
        c.watching = true;
    }

    void flush_child(Child& c)
    {
        while (!c.out.empty())
        {
            ControlMsg& m = c.out.front();
            iovec iov{&m.frame, sizeof(m.frame)};
            alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))];
            msghdr msg{};
            msg.msg_iov    = &iov;
            msg.msg_iovlen = 1;
            if (m.fd >= 0)
            {
                std::memset(cbuf, 0, sizeof(cbuf));
                msg.msg_control    = cbuf;
                msg.msg_controllen = sizeof(cbuf);
                cmsghdr* cm   = CMSG_FIRSTHDR(&msg);
                cm->cmsg_level = SOL_SOCKET;
                cm->cmsg_type  = SCM_RIGHTS;
                cm->cmsg_len   = CMSG_LEN(sizeof(int));
                std::memcpy(CMSG_DATA(cm), &m.fd, sizeof(int));
            }
            const ssize_t n = ::sendmsg(c.control, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0)
            {
                c.out.pop_front();
                continue;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                watch_control(c);
                return;
            }
            c.out.clear();   // the child is gone; its reap follows
            break;
        }
        if (c.watching && loop)
        {
            loop->remove_io(c.control);
            c.watching = false;
        }
    }

    void tell_others(const Child& about, std::uint8_t op)
    {
        for (auto& c : children)
        {
            if (c.pid == about.pid)
                continue;
            ControlMsg m{};
            m.frame.magic      = kControlMagic;
            m.frame.op         = op;
            m.frame.slot       = static_cast<std::uint16_t>(about.slot);
            m.frame.generation = about.generation;
            m.fd               = op == op_open ? about.inbox_w : -1;
            c.out.push_back(m);
            flush_child(c);
        }
    }

    void check_starting()
    {
        const long long now = now_ms();
        for (auto& c : children)
        {
            if (c.warned || now - c.forked_ms < 5000)
                continue;
            auto e = dir.get(c.slot);
            if (e && e->generation == c.generation && e->state == NodeDirectory::State::starting)
            {
                warn("ipc: process '{}' (pid={}) has not started for {} s — not a target of gather",
                     e->name, c.pid, (now - c.forked_ms) / 1000);
                c.warned = true;
            }
            else if (e)
                c.warned = true;   // started (or retiring): nothing to say any more
        }
    }
};

// ─── IpcReply ────────────────────────────────────────────────────────────────

IpcReply::~IpcReply()
{
    if (active_)
        answer(static_cast<std::uint8_t>(IpcError::failed), {});
}

IpcReply::IpcReply(IpcReply&& o) noexcept
    : core_(std::move(o.core_)), slot_(o.slot_), generation_(o.generation_), id_(o.id_),
      timeout_ms_(o.timeout_ms_), type_(std::move(o.type_)), active_(o.active_)
{
    o.active_ = false;
}

IpcReply& IpcReply::operator=(IpcReply&& o) noexcept
{
    if (this != &o)
    {
        if (active_)
            answer(static_cast<std::uint8_t>(IpcError::failed), {});
        core_       = std::move(o.core_);
        slot_       = o.slot_;
        generation_ = o.generation_;
        id_         = o.id_;
        timeout_ms_ = o.timeout_ms_;
        type_       = std::move(o.type_);
        active_     = o.active_;
        o.active_   = false;
    }
    return *this;
}

void IpcReply::send(std::string body)
{
    answer(static_cast<std::uint8_t>(IpcError::none), std::move(body));
}

void IpcReply::fail()
{
    answer(static_cast<std::uint8_t>(IpcError::failed), {});
}

void IpcReply::answer(std::uint8_t status, std::string body)
{
    if (!active_)
        return;
    active_ = false;
    if (auto c = core_.lock())
    {
        try { c->reply(slot_, generation_, id_, timeout_ms_, type_, status, std::move(body)); }
        catch (...) {}
    }
}

// ─── Ipc ─────────────────────────────────────────────────────────────────────

Ipc::Ipc() : core_(std::make_shared<detail::IpcCore>()) {}
Ipc::~Ipc() = default;

bool Ipc::connected() const noexcept { return core_->connected; }
void Ipc::enable() noexcept { core_->enabled = true; }
bool Ipc::enabled() const noexcept { return core_->enabled; }
const Ipc::Stats& Ipc::stats() const noexcept { return core_->stats; }

IpcPeer Ipc::self() const { return core_->me; }

std::vector<IpcPeer> Ipc::peers(unsigned roles) const
{
    auto& c = *core_;
    std::vector<IpcPeer> out;
    if (!c.connected)
    {
        if (role_in(c.me.role, roles))
            out.push_back(c.me);
        return out;
    }
    bool self_seen = false;
    for (const auto& e : c.dir.list())
    {
        const bool self = e.slot == c.me.slot && e.generation == c.me.generation;
        if (!self && e.state != NodeDirectory::State::live)
            continue;
        if (!role_in(e.role, roles))
            continue;
        if (self)
        {
            self_seen = true;
            out.push_back(c.me);
            continue;
        }
        IpcPeer p;
        p.slot = e.slot; p.role = e.role; p.name = e.name; p.number = e.number;
        p.pid = e.pid;   p.generation = e.generation;
        out.push_back(std::move(p));
    }
    if (!self_seen && role_in(c.me.role, roles))
        out.push_back(c.me);
    return out;
}

void Ipc::on(std::string type, Handler handler)
{
    if (type.empty() || type.size() > kMaxType || !handler)
        return;
    core_->handlers[std::move(type)] = std::move(handler);
}

void Ipc::off(std::string_view type)
{
    core_->handlers.erase(std::string(type));
}

bool Ipc::send(int slot, std::string_view type, std::string body)
{
    auto& c = *core_;
    if (type.empty() || type.size() > kMaxType || body.size() > kMaxMessage || c.stopping)
        return false;
    if (slot == c.me.slot)
    {
        std::weak_ptr<detail::IpcCore> w = core_;
        c.defer([w, type = std::string(type), body = std::move(body)]() mutable {
            if (auto core = w.lock())
                core->deliver_message(core->me, k_oneway, 0, 0, type, std::move(body));
        });
        return true;
    }
    if (!c.connected)
        return false;
    auto e = c.dir.get(slot);
    if (!e)
        return false;
    auto frames = make_frames(k_oneway, 0, c.me.slot, c.me.generation, 0, 0, type, body);
    return c.enqueue(slot, e->generation, std::move(frames), kHoldMs, 0) == IpcError::none;
}

void Ipc::request(int slot, std::string_view type, std::string body, RequestDone done,
                  std::chrono::milliseconds timeout)
{
    auto& c = *core_;
    auto fail_later = [&](IpcError e) {
        detail::IpcCore::bump(c.stats.requests, type, ipc_error_name(e));
        if (!done)
            return;
        if (!c.loop)
            return done({e, {}});
        c.defer([done = std::move(done), e]() mutable { done({e, {}}); });
    };

    if (type.empty() || type.size() > kMaxType)
        return fail_later(IpcError::failed);
    if (body.size() > kMaxMessage)
        return fail_later(IpcError::too_large);
    if (c.stopping)
        return fail_later(IpcError::stopping);
    if (!c.loop)
        return fail_later(IpcError::unreachable);

    std::uint32_t generation = c.me.generation;
    const bool self = slot == c.me.slot;
    if (!self)
    {
        if (!c.connected)
            return fail_later(IpcError::unreachable);
        auto e = c.dir.get(slot);
        if (!e)
            return fail_later(IpcError::gone);
        generation = e->generation;
    }

    const std::uint32_t id = c.new_id();
    const auto ms = std::max<long long>(1, timeout.count());
    std::weak_ptr<detail::IpcCore> w = core_;
    // The timer first: if it cannot be had, nothing is left waiting for it.
    const auto timer = c.loop->add_timer(std::chrono::milliseconds(ms), [w, id] {
        if (auto core = w.lock())
            core->on_timeout(id);
    }, /*repeat=*/false);
    auto& p = c.pending[id];
    p.slot       = slot;
    p.generation = generation;
    p.type       = std::string(type);
    p.done       = std::move(done);
    p.timer      = timer;

    const auto timeout_ms = static_cast<std::uint32_t>(std::min<long long>(ms, UINT32_MAX));
    if (self)
    {
        c.defer([w, id, timeout_ms, type = std::string(type), body = std::move(body)]() mutable {
            if (auto core = w.lock())
                core->deliver_message(core->me, k_request, id, timeout_ms, type, std::move(body));
        });
        return;
    }

    auto frames = make_frames(k_request, 0, c.me.slot, c.me.generation, id, timeout_ms, type, body);
    const IpcError e = c.enqueue(slot, generation, std::move(frames), ms, id);
    if (e != IpcError::none)
        c.defer([w, id, e] {
            if (auto core = w.lock())
                core->complete(id, {e, {}});
        });
}

void Ipc::gather(unsigned roles, std::string_view type, std::string body, GatherDone done,
                 std::chrono::milliseconds timeout)
{
    struct State
    {
        IpcGathered result;
        std::size_t remaining{0};
        GatherDone  done;
    };
    auto targets = peers(roles);
    auto st = std::make_shared<State>();
    st->remaining = targets.size();
    st->done = std::move(done);

    if (targets.empty())
    {
        auto& c = *core_;
        if (!c.loop)
            return st->done ? st->done({}) : void();
        c.defer([st] { if (st->done) st->done(std::move(st->result)); });
        return;
    }

    std::weak_ptr<detail::IpcCore> w = core_;
    const std::string t(type);
    for (const auto& peer : targets)
    {
        request(peer.slot, type, body, [st, peer, w, t](IpcResult r) {
            if (r.ok())
                st->result.replies.emplace_back(peer, std::move(r.body));
            else
            {
                st->result.missing.emplace_back(peer, r.error);
                if (auto core = w.lock())
                    detail::IpcCore::bump(core->stats.gather_missing, t, ipc_error_name(r.error));
            }
            if (--st->remaining == 0 && st->done)
                st->done(std::move(st->result));
        }, timeout);
    }
}

void Ipc::broadcast(unsigned roles, std::string_view type, std::string body)
{
    const auto self_slot = core_->me.slot;
    for (const auto& peer : peers(roles))
        if (peer.slot != self_slot)
            send(peer.slot, type, body);
}

// ─── Ipc: master ─────────────────────────────────────────────────────────────

bool Ipc::master_create(Logger& log)
{
    auto& c = *core_;
    c.log = &log;
    if (!c.enabled)
        return false;
    if (!c.dir.create())
    {
        log.warn("ipc: no shared memory for the process directory — processes run without channels");
        c.enabled = false;
        return false;
    }
    c.is_master = true;
    return true;
}

bool Ipc::master_prepare(ProcessRole role, std::string_view name, int number)
{
    auto& c = *core_;
    if (!c.is_master)
        return false;
    detail::IpcCore::Prepared p;
    p.generation = c.next_generation++;
    if (c.next_generation == 0)
        c.next_generation = 1;
    p.slot = c.dir.reserve(role, number, name, p.generation);
    if (p.slot < 0)
    {
        c.warn("ipc: the process directory is full — '{}' runs without channels", name);
        return false;
    }
    if (!make_pair(p.inbox) || !make_pair(p.control))
    {
        const int err = errno;
        close_fd(p.inbox[0]); close_fd(p.inbox[1]);
        close_fd(p.control[0]); close_fd(p.control[1]);
        c.dir.release(p.slot);
        c.warn("ipc: socketpair failed ({}) — '{}' runs without channels", std::strerror(err), name);
        return false;
    }
    p.valid  = true;
    p.role   = role;
    p.number = number;
    p.name   = std::string(name);
    c.prep   = std::move(p);
    return true;
}

void Ipc::master_forked_child()
{
    auto& c = *core_;
    if (!c.is_master)
        return;
    c.is_master = false;
    c.loop = nullptr;                 // the master's loop: never touched from here
    // What the master may have had of its own is not the child's: its timers
    // live on the master's loop (not cancelled — that is not ours either), its
    // requests wait for answers addressed to the master.
    c.master_timer = c.defer_timer = c.beat_timer = EventLoop::kInvalidTimer;
    c.deferred.clear();
    c.pending.clear();
    c.held.clear();
    c.parts.clear();
    c.links.clear();
    c.stats = {};
    const bool mine = c.prep.valid;

    // What the master holds for the others: their inboxes are ours to write to;
    // their control pairs are not ours at all.
    for (auto& ch : c.children)
    {
        close_fd(ch.control);
        if (mine)
        {
            auto& l = c.links[ch.slot];
            l.generation = ch.generation;
            l.fd = ch.inbox_w;
        }
        else
            close_fd(ch.inbox_w);
    }
    c.children.clear();

    if (!mine)
        return;
    auto& p = c.prep;
    close_fd(p.inbox[0]);             // our own inbox: written to by the others only
    close_fd(p.control[0]);           // the master's end
    c.inbox_fd   = p.inbox[1];
    c.control_fd = p.control[1];
    c.me.slot       = p.slot;
    c.me.role       = p.role;
    c.me.name       = p.name;
    c.me.number     = p.number;
    c.me.pid        = ::getpid();
    c.me.generation = p.generation;
    c.connected = true;
    c.prep = {};
}

void Ipc::master_forked_parent(pid_t pid)
{
    auto& c = *core_;
    if (!c.prep.valid)
        return;
    auto& p = c.prep;
    c.dir.assign(p.slot, pid);
    close_fd(p.inbox[1]);
    close_fd(p.control[1]);

    detail::IpcCore::Child ch;
    ch.pid        = pid;
    ch.slot       = p.slot;
    ch.generation = p.generation;
    ch.inbox_w    = p.inbox[0];
    ch.control    = p.control[0];
    ch.forked_ms  = now_ms();
    c.prep = {};

    c.children.push_back(std::move(ch));
    c.tell_others(c.children.back(), op_open);
}

void Ipc::master_fork_failed()
{
    auto& c = *core_;
    if (!c.prep.valid)
        return;
    auto& p = c.prep;
    close_fd(p.inbox[0]); close_fd(p.inbox[1]);
    close_fd(p.control[0]); close_fd(p.control[1]);
    c.dir.release(p.slot);
    c.prep = {};
}

void Ipc::master_retire(pid_t pid)
{
    auto& c = *core_;
    if (auto* ch = c.child_by_pid(pid))
        c.dir.retire(ch->slot);
}

void Ipc::master_reaped(pid_t pid)
{
    auto& c = *core_;
    auto it = std::find_if(c.children.begin(), c.children.end(),
                           [pid](const auto& ch) { return ch.pid == pid; });
    if (it == c.children.end())
        return;
    detail::IpcCore::Child dead = std::move(*it);
    c.children.erase(it);
    c.dir.release(dead.slot);

    // An "open" not yet delivered would pass a descriptor about to be closed.
    for (auto& ch : c.children)
    {
        auto& q = ch.out;
        q.erase(std::remove_if(q.begin(), q.end(), [&](const auto& m) {
                    return m.frame.op == op_open && m.frame.slot == dead.slot
                        && m.frame.generation == dead.generation;
                }),
                q.end());
    }
    c.tell_others(dead, op_close);

    if (dead.watching && c.loop)
        c.loop->remove_io(dead.control);
    close_fd(dead.inbox_w);
    close_fd(dead.control);
}

void Ipc::master_attach(EventLoop& loop)
{
    auto& c = *core_;
    if (!c.is_master)
        return;
    c.loop = &loop;
    for (auto& ch : c.children)
        c.flush_child(ch);
    std::weak_ptr<detail::IpcCore> w = core_;
    c.master_timer = loop.add_timer(std::chrono::seconds(1), [w] {
        if (auto core = w.lock())
            core->check_starting();
    });
}

void Ipc::master_detach()
{
    auto& c = *core_;
    if (!c.is_master || !c.loop)
        return;
    for (auto& ch : c.children)
        if (ch.watching)
        {
            c.loop->remove_io(ch.control);
            ch.watching = false;
        }
    if (c.master_timer != EventLoop::kInvalidTimer)
        c.loop->cancel_timer(c.master_timer);
    c.master_timer = EventLoop::kInvalidTimer;
    c.loop = nullptr;
}

// ─── Ipc: a process with a loop ──────────────────────────────────────────────

void Ipc::attach(EventLoop& loop, Logger& log, ProcessRole role, std::string_view name)
{
    auto& c = *core_;
    c.loop     = &loop;
    c.log      = &log;
    c.attached = true;
    c.stopping = false;
    if (!c.connected)
    {
        c.me = {};
        c.me.role = role;
        c.me.name = std::string(name);
        c.me.pid  = ::getpid();
        return;
    }

    std::weak_ptr<detail::IpcCore> w = core_;
    loop.add_io(c.inbox_fd, EPOLLIN, [w](std::uint32_t) {
        if (auto core = w.lock())
            core->on_inbox();
    });
    loop.add_io(c.control_fd, EPOLLIN, [w](std::uint32_t) {
        if (auto core = w.lock())
            core->on_control();
    });
    c.beat_timer = loop.add_timer(std::chrono::seconds(1), [w] {
        if (auto core = w.lock())
            core->on_beat();
    });
    c.dir.stamp(c.me.slot);
    // Who is on the node may have changed since fork: learn it now. The inbox
    // waits for the loop — the handlers are registered after attach, and a
    // message read now would find none. What arrived since fork is reported
    // by the loop as readable all the same (EPOLL_CTL_ADD checks the state,
    // edge-triggered too).
    c.on_control();
}

void Ipc::ready()
{
    auto& c = *core_;
    if (c.connected && c.attached && !c.stopping)
        c.dir.mark(c.me.slot, NodeDirectory::State::live);
}

void Ipc::detach()
{
    auto& c = *core_;
    if (!c.attached)
        return;
    c.stopping = true;
    if (c.connected)
        c.dir.mark(c.me.slot, NodeDirectory::State::retiring);

    // Work already deferred is finished — answers to ourselves, completions —
    // and what is still waiting ends as "stopping", so that every request gets
    // exactly one call of its callback. A callback may ask again (it is told
    // "stopping", through the same queue): rounds until nothing is left, with
    // a bound against a callback that keeps asking.
    constexpr int kRounds = 16;
    for (int round = 0; round < kRounds; ++round)
    {
        c.run_deferred();
        std::vector<std::uint32_t> ids;
        for (const auto& [id, p] : c.pending)
            ids.push_back(id);
        for (auto id : ids)
            c.complete(id, {IpcError::stopping, {}});
        if (c.deferred.empty() && c.pending.empty())
            break;
    }
    if (!c.deferred.empty() || !c.pending.empty())
        c.warn("ipc: detached with {} callback(s) and {} request(s) left after {} rounds — "
               "a callback keeps asking while the process stops",
               c.deferred.size(), c.pending.size(), kRounds);
    c.handlers.clear();

    if (c.loop)
    {
        if (c.defer_timer != EventLoop::kInvalidTimer)
            c.loop->cancel_timer(c.defer_timer);
        if (c.beat_timer != EventLoop::kInvalidTimer)
            c.loop->cancel_timer(c.beat_timer);
        if (c.inbox_fd >= 0)
            c.loop->remove_io(c.inbox_fd);
        if (c.control_fd >= 0)
            c.loop->remove_io(c.control_fd);
        for (auto& [slot, l] : c.links)
            if (l.watching)
                c.loop->remove_io(l.fd);
    }
    c.defer_timer = c.beat_timer = EventLoop::kInvalidTimer;
    c.deferred.clear();
    // Closing the inbox now: whoever writes to us from here on learns at once
    // that we are gone, instead of waiting for the exit.
    close_fd(c.inbox_fd);
    close_fd(c.control_fd);
    for (auto& [slot, l] : c.links)
        close_fd(l.fd);
    c.links.clear();
    c.held.clear();
    c.parts.clear();
    c.attached = false;
    c.loop = nullptr;
}

} // namespace apostol
