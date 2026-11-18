#include "pg_resolver.hpp"

#include "apostol/event_loop.hpp"

#include <libpq-fe.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace apostol
{

// ── What the string asks for ────────────────────────────────────────────────

struct PgResolver::Plan
{
    enum class Kind { name, address, other };   // other: a socket directory, or empty

    std::vector<std::pair<std::string, std::string>> keep;  // every other option, as given
    std::vector<std::string> hosts;
    std::vector<Kind>        kinds;
    std::vector<std::string> ports;                        // one per host, one for all, or none
    std::vector<std::string> names;                        // the distinct names to look up
    std::vector<std::vector<std::string>> last;            // per name: its last addresses found
};

namespace
{

struct Answer
{
    std::vector<std::string> addrs;
    std::string              error;
};

std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    for (;;) {
        const auto comma = s.find(',', from);
        out.push_back(s.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos)
            return out;
        from = comma + 1;
    }
}

std::string join_list(const std::vector<std::string>& v)
{
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i)
            out += ',';
        out += v[i];
    }
    return out;
}

bool numeric_address(const std::string& h)
{
    unsigned char buf[sizeof(in6_addr)];
    return ::inet_pton(AF_INET, h.c_str(), buf) == 1 || ::inet_pton(AF_INET6, h.c_str(), buf) == 1;
}

// A conninfo value: quoted, with ' and \ escaped.
void append_option(std::string& out, std::string_view keyword, std::string_view value)
{
    if (!out.empty())
        out += ' ';
    out.append(keyword);
    out += "='";
    for (char c : value) {
        if (c == '\'' || c == '\\')
            out += '\\';
        out += c;
    }
    out += '\'';
}

const char* env(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

std::unique_ptr<PgResolver::Plan> make_plan(const std::string& conninfo)
{
    char* err = nullptr;
    PQconninfoOption* opts = PQconninfoParse(conninfo.c_str(), &err);
    if (!opts) {
        if (err)
            PQfreemem(err);
        return nullptr;   // libpq reports it at connect
    }

    auto plan = std::make_unique<PgResolver::Plan>();
    std::optional<std::string> host, hostaddr, port, service;
    for (const PQconninfoOption* o = opts; o->keyword; ++o) {
        if (!o->val)
            continue;
        const std::string_view kw = o->keyword;
        if (kw == "host")
            host = o->val;
        else if (kw == "hostaddr")
            hostaddr = o->val;
        else if (kw == "port")
            port = o->val;
        else if (kw == "service")
            service = o->val;
        else
            plan->keep.emplace_back(o->keyword, o->val);
    }
    PQconninfoFree(opts);

    // What libpq would take from the environment for an option not given.
    if (!service && env("PGSERVICE"))
        return nullptr;
    if (!hostaddr && env("PGHOSTADDR"))
        return nullptr;
    if (service || (hostaddr && !hostaddr->empty()))
        return nullptr;
    if (!host) {
        if (const char* e = env("PGHOST"))
            host = e;
        else
            return nullptr;
    }
    if (!port) {
        if (const char* e = env("PGPORT"))
            port = e;
    }

    plan->hosts = split_list(*host);
    if (port)
        plan->ports = split_list(*port);
    if (plan->ports.size() > 1 && plan->ports.size() != plan->hosts.size())
        return nullptr;

    for (const auto& h : plan->hosts) {
        using Kind = PgResolver::Plan::Kind;
        if (h.empty() || h[0] == '/' || h[0] == '@')
            plan->kinds.push_back(Kind::other);
        else if (numeric_address(h))
            plan->kinds.push_back(Kind::address);
        else {
            plan->kinds.push_back(Kind::name);
            if (std::find(plan->names.begin(), plan->names.end(), h) == plan->names.end())
                plan->names.push_back(h);
        }
    }
    if (plan->names.empty())
        return nullptr;
    plan->last.resize(plan->names.size());
    return plan;
}

// The string to connect with; nullopt — no host left. @p error names every
// name that did not resolve.
std::optional<std::string> build(const PgResolver::Plan& plan, const std::vector<Answer>& answers,
                                 std::string& error)
{
    using Kind = PgResolver::Plan::Kind;
    const bool per_host = plan.ports.size() > 1;

    std::vector<std::string> hosts, addrs, ports;
    for (std::size_t i = 0; i < plan.hosts.size(); ++i) {
        const std::string& h    = plan.hosts[i];
        const std::string  port = per_host ? plan.ports[i] : std::string();

        if (plan.kinds[i] != Kind::name) {
            hosts.push_back(h);
            addrs.push_back(plan.kinds[i] == Kind::address ? h : std::string());
            ports.push_back(port);
            continue;
        }
        const auto n = static_cast<std::size_t>(
            std::find(plan.names.begin(), plan.names.end(), h) - plan.names.begin());
        const Answer& a = answers.at(n);
        if (a.addrs.empty()) {
            if (error.find('"' + h + '"') == std::string::npos) {
                if (!error.empty())
                    error += "; ";
                error += "could not translate host name \"" + h + "\" to address: " + a.error;
            }
            continue;
        }
        for (const auto& addr : a.addrs) {
            hosts.push_back(h);
            addrs.push_back(addr);
            ports.push_back(port);
        }
    }
    if (hosts.empty())
        return std::nullopt;

    std::string out;
    for (const auto& [kw, val] : plan.keep)
        append_option(out, kw, val);
    append_option(out, "host", join_list(hosts));
    append_option(out, "hostaddr", join_list(addrs));
    if (per_host)
        append_option(out, "port", join_list(ports));
    else if (plan.ports.size() == 1)
        append_option(out, "port", plan.ports[0]);
    return out;
}

} // namespace

// ── The lookup ──────────────────────────────────────────────────────────────

struct PgResolver::Job
{
    std::vector<std::string> names;   // read-only once the thread runs
    std::mutex               mutex;
    std::vector<Answer>      answers;
    int                      efd = -1;

    ~Job()
    {
        if (efd >= 0)
            ::close(efd);
    }
};

namespace
{

// On the lookup's own thread: touches nothing but its arguments.
void look_up(const std::vector<std::string>& names, std::vector<Answer>& out)
{
    out.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;     // as libpq asks
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(names[i].c_str(), nullptr, &hints, &res);
        if (rc != 0) {
            out[i].error = ::gai_strerror(rc);
            continue;
        }
        for (const addrinfo* p = res; p; p = p->ai_next) {
            char buf[NI_MAXHOST];
            if (::getnameinfo(p->ai_addr, p->ai_addrlen, buf, sizeof buf, nullptr, 0, NI_NUMERICHOST) != 0)
                continue;
            if (std::find(out[i].addrs.begin(), out[i].addrs.end(), buf) == out[i].addrs.end())
                out[i].addrs.emplace_back(buf);
        }
        ::freeaddrinfo(res);
        if (out[i].addrs.empty())
            out[i].error = "no usable address";
    }
}

} // namespace

PgResolver::PgResolver(EventLoop& loop, std::string conninfo, ReadyHandler on_ready)
    : loop_(loop)
    , conninfo_(std::move(conninfo))
    , on_ready_(std::move(on_ready))
    , plan_(make_plan(conninfo_))
    , resolves_(plan_ != nullptr)
{
}

PgResolver::~PgResolver()
{
    // The thread finishes on its own and closes the eventfd with the job.
    if (job_) {
        try {
            loop_.remove_io(job_->efd);
        } catch (...) {
        }
    }
}

std::optional<std::string> PgResolver::conninfo()
{
    if (!plan_)
        return conninfo_;

    // No answer yet, or the one held has just failed a connect: wait for a
    // fresh one rather than hand out what is known not to work.
    const bool wait = !resolved_ || stale_;
    const bool due  = wait || std::chrono::steady_clock::now() >= refresh_at_;
    if (due && !job_)
        start();
    if (!job_)
        // No thread to look up on: the answer held, or libpq looks up
        // itself, as it always did.
        return resolved_ ? resolved_ : std::optional<std::string>(conninfo_);
    if (wait) {
        awaited_ = true;
        return std::nullopt;
    }
    return resolved_;
}

void PgResolver::start()
{
    auto job   = std::make_shared<Job>();
    job->names = plan_->names;
    job->efd   = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (job->efd < 0)
        return;

    try {
        loop_.add_io(job->efd, EPOLLIN, [this](uint32_t) { on_done(); });
    } catch (...) {
        return;
    }

    try {
        // The thread co-owns the job: the resolver may be gone by the time
        // getaddrinfo() returns.
        std::thread([job] {
            // Nothing may escape a thread's function (std::terminate): a
            // lookup that throws ends as one that found nothing, and the
            // loop hears of it either way.
            try {
                std::vector<Answer> answers;
                try {
                    look_up(job->names, answers);
                } catch (const std::exception& e) {
                    answers.clear();
                    answers.resize(job->names.size());
                    for (auto& a : answers)
                        a.error = e.what();
                }
                std::lock_guard lock(job->mutex);
                job->answers = std::move(answers);
            } catch (...) {
            }
            const std::uint64_t one = 1;
            (void)!::write(job->efd, &one, sizeof one);
        }).detach();
    } catch (...) {
        try {
            loop_.remove_io(job->efd);
        } catch (...) {
        }
        return;
    }

    job_   = std::move(job);
    stale_ = false;
}

void PgResolver::on_done() noexcept
{
    if (!job_)
        return;

    std::uint64_t v = 0;
    (void)!::read(job_->efd, &v, sizeof v);
    try {
        loop_.remove_io(job_->efd);
    } catch (...) {
    }

    std::vector<Answer> answers;
    {
        std::lock_guard lock(job_->mutex);
        answers = std::move(job_->answers);
    }
    job_.reset();

    using namespace std::chrono;
    Outcome out;
    out.awaited = std::exchange(awaited_, false);
    const auto now = steady_clock::now();
    try {
        // A name that failed this time keeps its last addresses: one refused
        // answer of several does not take a host out of the list. The others
        // are remembered as found.
        answers.resize(plan_->names.size());
        bool failed = false;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            if (!answers[i].addrs.empty()) {
                plan_->last[i] = answers[i].addrs;
                continue;
            }
            failed = true;
            if (plan_->last[i].empty())
                continue;   // build() names it
            if (!out.error.empty())
                out.error += "; ";
            out.error += "could not translate host name \"" + plan_->names[i] + "\" to address: "
                       + answers[i].error + " — the addresses found before are used";
            answers[i].addrs = plan_->last[i];
        }
        if (auto s = build(*plan_, answers, out.error))
            resolved_ = std::move(s);
        else {
            out.ok   = false;
            out.kept = resolved_.has_value();
        }
        refresh_at_ = now + (failed ? kRetry : kMaxAge);
    } catch (const std::exception& e) {
        out.ok      = false;
        out.kept    = resolved_.has_value();
        out.error   = e.what();
        refresh_at_ = now + kRetry;
    }

    if (on_ready_) {
        try {
            on_ready_(out);
        } catch (...) {
        }
    }
}

} // namespace apostol
