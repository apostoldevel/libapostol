// pg_nul_test — a statement holding a NUL byte, in this process, against a
// pool whose server does not exist (127.0.0.1, port 1): nothing here needs a
// database, since such a statement must never leave the pool.
//
//   1. execute() with an error handler: the handler is called once with
//      PgPool::nul_refusal — from the loop, never from inside execute() — and
//      no socket is opened for it;
//   2. execute() without an error handler: the result handler gets one result,
//      not ok(), whose error message is nul_refusal;
//   3. cancel() before the loop turns: no handler is called;
//   4. listener_execute(): refused the same way, and no listener is started;
//   5. a handler that sends the same statement again on every refusal: each
//      refusal comes on a turn of its own, and the loop keeps running
//      (a timer set alongside still fires);
//   6. detach() after a refusal: no handler is called;
//   7. a handler that throws (error or result): the exception stays in the
//      pool, and the refusal after it is delivered on the same turn;
//   8. control — the same calls without the NUL byte are not refused: the
//      pool and the listener open a socket for them, inside the call.
//
// Usage: pg_nul_test

#include "apostol/event_loop.hpp"
#include "apostol/pg.hpp"

#include <dirent.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace apostol;
using namespace std::chrono_literals;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("pg_nul_test: FAIL %s\n", what.c_str());
    } else {
        std::printf("pg_nul_test: ok   %s\n", what.c_str());
    }
}

int open_fds()
{
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d))
            ++n;
        closedir(d);
    }
    return n;
}

const char* kConninfo = "host=127.0.0.1 port=1 connect_timeout=1";

std::string with_nul()
{
    std::string sql = "SELECT 'abc";
    sql.push_back('\0');
    sql += "def'";
    return sql;
}

// A lazy pool: it opens nothing until a statement is sent.
struct Fixture
{
    EventLoop loop;
    PgPool    pool{loop, kConninfo, 1, 1};

    Fixture() { pool.set_lazy(true); pool.start(); }

    void turn(std::chrono::milliseconds d = 50ms) { loop.run_for(d); }
};

void error_handler()
{
    Fixture f;
    const int fds = open_fds();

    int calls = 0;
    std::string error;
    f.pool.execute(with_nul(),
        [&](std::vector<PgResult>) { ++calls; error = "result handler called"; },
        [&](std::string_view e) { ++calls; error = std::string(e); });

    check(calls == 0, "execute: not called from inside execute()");
    check(f.pool.outstanding() == 1, "execute: counted by outstanding() until told");
    f.turn();
    check(calls == 1, "execute: error handler called once");
    check(error == PgPool::nul_refusal, "execute: error is nul_refusal (got '" + error + "')");
    check(f.pool.outstanding() == 0, "execute: nothing outstanding after");
    check(f.pool.connection_count() == 0, "execute: no pool connection");
    check(open_fds() == fds, "execute: no socket opened");
}

void result_handler_only()
{
    Fixture f;
    int calls = 0;
    bool ok = true;
    std::string error;
    f.pool.execute(with_nul(), [&](std::vector<PgResult> results) {
        ++calls;
        if (results.size() == 1) {
            ok    = results[0].ok();
            error = results[0].error_message();
        }
    });
    f.turn();
    check(calls == 1, "no error handler: result handler called once");
    check(!ok, "no error handler: the result is not ok()");
    check(error == PgPool::nul_refusal, "no error handler: error_message() is nul_refusal");
}

void canceled()
{
    Fixture f;
    int calls = 0;
    auto id = f.pool.execute(with_nul(),
        [&](std::vector<PgResult>) { ++calls; },
        [&](std::string_view) { ++calls; });
    check(f.pool.cancel(id), "cancel: found");
    f.turn();
    check(calls == 0, "cancel: no handler called");
    check(f.pool.outstanding() == 0, "cancel: nothing outstanding");
}

void listener()
{
    Fixture f;
    const int fds = open_fds();
    int calls = 0;
    std::string error;
    f.pool.listener_execute(with_nul(),
        [&](std::vector<PgResult>) { ++calls; error = "result handler called"; },
        [&](std::string_view e) { ++calls; error = std::string(e); });
    check(calls == 0, "listener_execute: not called from inside the call");
    f.turn();
    check(calls == 1, "listener_execute: error handler called once");
    check(error == PgPool::nul_refusal, "listener_execute: error is nul_refusal (got '" + error + "')");
    check(open_fds() == fds, "listener_execute: no listener started");
}

void detached()
{
    Fixture f;
    int calls = 0;
    f.pool.execute(with_nul(), [&](std::vector<PgResult>) { ++calls; }, [&](std::string_view) { ++calls; });
    f.pool.detach();
    f.turn();
    check(calls == 0, "detach: no handler called after detach()");
    check(f.pool.outstanding() == 0, "detach: nothing outstanding");
}

void handler_throws()
{
    Fixture f;
    int second = 0;
    f.pool.execute(with_nul(), {}, [&](std::string_view) { throw std::runtime_error("handler"); });
    f.pool.execute(with_nul(), {}, [&](std::string_view) { ++second; });
    bool escaped = false;
    try { f.turn(); } catch (...) { escaped = true; }
    check(!escaped, "handler throws: the exception does not leave the loop");
    check(second == 1, "handler throws: the next refusal is delivered on the same turn");
    check(f.pool.outstanding() == 0, "handler throws: nothing outstanding");

    // The same for a caller with a result handler only.
    int third = 0;
    f.pool.execute(with_nul(), [&](std::vector<PgResult>) { throw std::runtime_error("result handler"); });
    f.pool.execute(with_nul(), [&](std::vector<PgResult>) { ++third; });
    escaped = false;
    try { f.turn(); } catch (...) { escaped = true; }
    check(!escaped, "result handler throws: the exception does not leave the loop");
    check(third == 1, "result handler throws: the next refusal is delivered on the same turn");
    check(f.pool.outstanding() == 0, "result handler throws: nothing outstanding");
}

void resent_from_handler()
{
    Fixture f;
    int calls = 0;
    int ticks = 0;
    std::function<void()> send;
    send = [&] {
        f.pool.execute(with_nul(), {}, [&](std::string_view) {
            ++calls;
            send();   // the same statement again, every time
        });
    };
    send();
    f.loop.add_timer(5ms, [&] { ++ticks; }, /*repeat=*/true);
    f.turn(100ms);
    check(ticks > 0, "resent: the loop keeps running (timer fired " + std::to_string(ticks) + " times)");
    check(calls > 1, "resent: each refusal delivered (" + std::to_string(calls) + ")");
}

void control()
{
    {
        Fixture f;
        int calls = 0;
        std::string error;
        f.pool.execute("SELECT 'abcdef'",
            [&](std::vector<PgResult>) { ++calls; },
            [&](std::string_view e) { ++calls; error = std::string(e); });
        // Inside the call, before the loop turns: no reconnect gap to fall in.
        check(f.pool.connection_count() > 0, "control execute: a connection is opened");
        f.turn();
        check(error != PgPool::nul_refusal, "control execute: not refused");
    }
    {
        Fixture f;
        const int fds = open_fds();
        std::string error;
        f.pool.listener_execute("SELECT 'abcdef'", {}, [&](std::string_view e) { error = std::string(e); });
        check(open_fds() > fds, "control listener_execute: a listener socket is opened");
        f.turn();
        check(error != PgPool::nul_refusal, "control listener_execute: not refused");
    }
}

} // namespace

int main()
{
    error_handler();
    result_handler_only();
    canceled();
    listener();
    detached();
    handler_throws();
    resent_from_handler();
    control();

    if (g_failed) {
        std::printf("pg_nul_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("pg_nul_test: all passed\n");
    return 0;
}
