[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

# libapostol (Apostol)

**Apostol** is a high-performance C++20 framework for developing backend
applications and system services on Linux with direct access to PostgreSQL.

> A-POST-OL → **A**synchronous **POST** **O**rchestration **L**oop — a single
> event loop for HTTP and PostgreSQL.

The HTTP server and PostgreSQL sockets live in a **single event loop**. Data
flows directly between the HTTP server and the database — no intermediate
scripting layers (PHP, Python, etc.). This minimizes latency and overhead and
provides predictable performance.

> **libapostol is the Apostol framework itself.**
> The [apostol](https://github.com/apostoldevel/apostol) repository is an
> example assembly built from libapostol and reusable modules.

---

## Quick Start

Create a new project from the libapostol template:

```bash
bash <(curl -sL https://raw.githubusercontent.com/apostoldevel/libapostol/master/install.sh)
```

Build and run (assumes the default project name `myapp` — adjust if you chose a different name):

```bash
cd myapp
./configure --debug
cmake --build cmake-build-debug --parallel $(nproc)

# For local development: change "prefix" in conf/default.json to "."
mkdir -p logs
./cmake-build-debug/myapp -p . -c conf/default.json

curl http://localhost:4977/api/v1/ping   # → {"ok":true,"message":"OK"}
curl http://localhost:4977/docs          # → Swagger UI
```

For a detailed walkthrough see **[Getting Started](https://github.com/apostoldevel/libapostol/wiki/Getting-Started)** in the wiki.

---

## REST API with Built-in Swagger UI

Inherit from `RoutedModule`, define routes — get interactive API documentation
for free. No external tooling, code generation, or hand-written YAML required.

```cpp
void MyService::init_routes()
{
    routes_.add_route("GET", "/api/v1/ping",
        [](const HttpRequest&, HttpResponse& resp, const PathParams&) {
            resp.set_status(HttpStatus::ok)
                .set_body(R"({"ok":true,"message":"OK"})",
                          "application/json; charset=utf-8");
        })
        .summary("Ping healthcheck")
        .tag("General")
        .response(200, "OK");

    routes_.add_route("GET", "/api/v1/users/{id}",
        [](const HttpRequest&, HttpResponse& resp, const PathParams& params) {
            auto id = params["id"];
            // ...
        })
        .summary("Get user by ID")
        .tag("Users")
        .param("id", "path", "string", true, "User identifier")
        .response(200, "User found")
        .response(404, "Not found");
}
```

Every route registered via `RouteBuilder` is automatically exposed as:

- **`/docs`** — interactive [Swagger UI](https://swagger.io/tools/swagger-ui/) (loaded from CDN, no static files needed)
- **`/docs/api.json`** — OpenAPI 3.0 specification (JSON)
- **`/docs/api.yaml`** — OpenAPI 3.0 specification (YAML)

See [Creating Modules](https://github.com/apostoldevel/libapostol/wiki/Creating-Modules) for the full `RouteBuilder` API reference.

---

## Features

| Category   | Components                                                                                     |
|------------|------------------------------------------------------------------------------------------------|
| Core       | Logger (gzip rotation), Config (JSON, hot-reload, env), EventLoop (epoll+timerfd+signalfd), Process model (master/worker/helper/single), Crash handler (backtrace, addr2line), Settings |
| Networking | TCP (TLS via OpenSSL), HTTP/1.1 (llhttp, keep-alive, chunked), WebSocket (RFC 6455), UDP      |
| Clients    | FetchClient (CurlClient/HttpClient), SmtpClient, TcpClient, UdpClient, HttpProxy              |
| PostgreSQL | Async pool (libpq+epoll), LISTEN/NOTIFY, deferred dispatch (`exec_sql`), PG utils              |
| REST API   | RoutedModule + RouteBuilder fluent API, path parameters, wildcard routes, OpenAPI 3.0 spec generation, built-in Swagger UI |
| Security   | JWT verification (HS/RS/ES/PS via jwt-cpp), OAuth2 providers, BotSession, ServiceToken, CORS                  |
| Utilities  | Base64 (RFC 4648), File utils (SHA256, MIME), HTTP utils, RouteManager, SiteConfig              |

## Technology Stack

| Library             | Version  | Type            |
|---------------------|----------|-----------------|
| nlohmann/json       | v3.11.3  | FetchContent    |
| {fmt}               | v11.1.4  | FetchContent    |
| llhttp              | v9.2.1   | FetchContent    |
| OpenSSL             | system   | optional        |
| libcurl             | system   | optional        |
| libpq (PostgreSQL)  | system   | optional        |
| zlib                | system   | required        |

## Feature Flags

| Flag              | Default | What it enables                                    |
|-------------------|---------|----------------------------------------------------|
| `WITH_POSTGRESQL` | `ON`    | PgPool, pg_utils, pg_exec, BotSession, ServiceToken |
| `WITH_SSL`        | `ON`    | TLS, JWT verification, SmtpClient                  |
| `WITH_CURL`       | `ON`    | FetchClient uses CurlClient (async libcurl)        |

All three flags can be turned `OFF` for a minimal build with no external
optional dependencies.

---

## Template Projects

libapostol is a module constructor — pick the modules you need and assemble
them into a build. Two reference builds demonstrate the pattern:

### Apostol

**[apostol](https://github.com/apostoldevel/apostol)** — HTTP-to-PostgreSQL gateway.

| Module | Type | Description |
|--------|------|-------------|
| [PGHTTP](https://github.com/apostoldevel/module-PGHTTP) | Worker | HTTP → PL/pgSQL function dispatch |
| [WebServer](https://github.com/apostoldevel/module-WebServer) | Worker | Static file serving + Swagger UI |
| [PGFetch](https://github.com/apostoldevel/module-PGFetch) | Helper | LISTEN/NOTIFY → outgoing HTTP requests |

### Apostol CRM

[Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm] — full-stack backend with auth, REST API, file serving, WebSocket, and background processes.

---

## Modules

All modules are self-contained and reusable across projects:

### Workers (HTTP/WebSocket request handlers)

| Module | Description | Requirements |
|--------|-------------|--------------|
| [PGHTTP](https://github.com/apostoldevel/module-PGHTTP) | HTTP → PL/pgSQL function dispatch | WITH_POSTGRESQL |
| [WebServer](https://github.com/apostoldevel/module-WebServer) | Static file serving + Swagger UI | — |
| [AuthServer](https://github.com/apostoldevel/module-AuthServer) | OAuth 2.0 authorization server + JWT | WITH_POSTGRESQL + WITH_SSL |
| [AppServer](https://github.com/apostoldevel/module-AppServer) | Auth-aware REST API → PostgreSQL dispatch | WITH_POSTGRESQL + WITH_SSL |
| [FileServer](https://github.com/apostoldevel/module-FileServer) | HTTP file serving with JWT authentication | WITH_POSTGRESQL + WITH_SSL |
| [WebSocketAPI](https://github.com/apostoldevel/module-WebSocketAPI) | JSON-RPC + pub/sub over WebSocket | WITH_POSTGRESQL + WITH_SSL |
| [StreamServer](https://github.com/apostoldevel/process-StreamServer) | UDP datagram processing via PG | WITH_POSTGRESQL |

### Helpers (background modules in helper process)

| Module | Description | Requirements |
|--------|-------------|--------------|
| [PGFetch](https://github.com/apostoldevel/module-PGFetch) | LISTEN/NOTIFY → outgoing HTTP requests | WITH_POSTGRESQL |
| [PGFile](https://github.com/apostoldevel/module-PGFile) | LISTEN/NOTIFY → file sync (PG ↔ filesystem) | WITH_POSTGRESQL |

### Processes (independent background processes)

| Process | Description | Requirements |
|---------|-------------|--------------|
| [TaskScheduler](https://github.com/apostoldevel/process-TaskScheduler) | Cron-like job execution from `db.job` queue | WITH_POSTGRESQL |
| [ReportServer](https://github.com/apostoldevel/process-ReportServer) | LISTEN-driven report generation | WITH_POSTGRESQL |
| [MessageServer](https://github.com/apostoldevel/process-MessageServer) | Message dispatch (SMTP / FCM / HTTP API) | WITH_POSTGRESQL + WITH_SSL |
| [Replication](https://github.com/apostoldevel/process-Replication) | Master-slave data sync over HTTP | WITH_POSTGRESQL |

---

## Benchmark

**Apostol v2 vs v1 vs Python vs Node.js vs Go vs Nginx** — comparative
benchmark under identical Docker conditions.

### /ping — no database (keep-alive ON, 100 connections)

| Service | RPS | Latency p50 |
|---------|----:|------------:|
| Nginx (static return) | 566,000 | 111us |
| **Apostol v2** | **507,000** | **170us** |
| Go (net/http) | 211,000 | 0.45ms |
| Apostol v1 | 128,000 | 790us |
| Node.js (Fastify) | 102,000 | 0.95ms |
| Python (FastAPI) | 2,400 | 41ms |

### /db/ping — PostgreSQL round-trip (keep-alive ON, 100 connections)

| Service | RPS | Latency p50 |
|---------|----:|------------:|
| **Apostol v2** | **112,000** | **0.91ms** |
| Go | 72,000 | 1.07ms |
| Apostol v1 | 61,000 | 1.61ms |
| Node.js | 36,000 | 2.65ms |
| Python | 2,300 | 42ms |

**Key findings:**
- Apostol v2 is **4x faster** than v1 and **2.4x faster** than Go on /ping
- Apostol v2 reaches **90% of Nginx's throughput** on /ping with keep-alive (507K vs 566K) and **matches Nginx** without keep-alive (84K vs 84K at c100)
- At 1000 connections without keep-alive, Apostol v2 **surpasses Nginx** (81K vs 75K RPS) thanks to `SO_REUSEPORT`

> Full results, methodology, and analysis:
> [REST API Benchmark](https://github.com/apostoldevel/apostol/blob/master/doc/BENCHMARK.md).

---

## Usage

Add libapostol as a subdirectory (e.g. git submodule) and link against it:

```cmake
add_subdirectory(src/lib/libapostol)
target_link_libraries(my_app PRIVATE apostol)
```

Feature flags (`WITH_POSTGRESQL`, `WITH_SSL`, `WITH_CURL`) are propagated as
`PUBLIC` compile definitions, so consuming targets get them automatically.

### Configuration from the environment

Every string value in the JSON config may reference an environment variable —
`"${VAR}"` or `"$VAR"` — expanded every time the file is loaded, the SIGHUP
reload included. An unset variable expands to an empty string. Since expansion works on strings
only, a flag or a number that the deployment decides is written as a string,
and `Config::get_bool` / `Config::get_int` read it:

```json
{
  "module":   { "GatewayAPI": { "enable": "${GATEWAY_ENABLE}" } },
  "server":   { "port": "${PORT}" },
  "database": { "password": "${DB_PASSWORD}" }
}
```

`get_bool` accepts `true|false|1|0|yes|no|on|off` in any case; `get_int` a whole
decimal integer. An empty or blank string (the variable is not set) counts as
an absent key: the accessor with a default returns the default, the one without
throws `ConfigError`. Any other string throws `ConfigError` naming the key and
the value — nothing is guessed (`GATEWAY_ENABLE=maybe` → `config key
'module.GatewayAPI.enable' must be a bool (…), got string 'maybe'`). Where that
lands depends on who reads the key: the master's own settings fail the start;
a module flag is read when a worker starts, so the worker exits and the master
respawns it with a backoff — the error line repeats until the variable is fixed.

### One active copy per background role

Two copies of an application on one database — two containers, two pods, a
rolling update — run every background role twice: two schedulers take the same
job, two PGFetch send the same request. `"leader": true` keeps one copy of each
role active and the rest waiting:

```json
{
  "node":    "${NODE_NAME}",
  "process": { "helper": true, "leader": "${LEADER}", "leader_interval": 5 },
  "module":  { "LicenseServer": { "enable": true, "leader": false } }
}
```

- `process.leader` — applies to the helper process and to every custom process;
  `module.<Name>.leader` overrides it for one custom process. Workers are not
  affected. Either key is checked with the file — a value that is not a bool,
  or `true` without `postgres.helper`, refuses a start, a reload and `-t`.
- Before its modules start, the process takes `pg_try_advisory_lock` on a
  dedicated connection (the `postgres.helper` settings), key
  `"<application>/<role>"`. Holding it — it starts; otherwise it waits as
  `… process (standby)` with one connection and no pool, asking every
  `leader_interval` seconds, and logs who holds the role (the holder's
  `application_name`: `"<application> <role> <node>:<pid>"`).
- The lock connection must reach PostgreSQL in session mode — directly, or a
  pgbouncer pool in `session` mode. Under transaction pooling a session lock
  guarantees nothing: two copies can be active at once, and the lock can stay
  behind in pgbouncer's server connection after the process is gone, blocking
  the role for everyone. The leader reports a changed server session and steps
  down, but that is a diagnosis, not a guard.
- Losing the lock — the connection breaks, the server does not answer within
  max(3 × interval, 15 s), or the lock is gone — stops the modules and ends the
  process with code 1; the master starts a new one, which waits as a standby.
  The session sets `idle_session_timeout` (PostgreSQL 14+) to four times that
  timeout, so a leader that vanished without closing its socket frees the role
  in about a minute, after it has stepped down on its own clock.
- Timer-driven work (`heartbeat`) of a leader past a stall waits until the lock
  is confirmed again, and before each beat the lock's socket is asked whether
  the server has closed it: a session killed while the process stood is a loss
  before the beat, not after. Work arriving on sockets — a NOTIFY read in the same wake-up
  as the loss — cannot be ordered that way: where a duplicate costs money, claim
  the row in the database as well.

`node` names this copy — `Application::node_id()`: the configured value, else
`$NODE_NAME` (in Kubernetes, the pod name from the downward API), else the host
name; `node_process_id()` adds `:<pid>`. Only a string counts: an application
that keeps an object under `"node"` (cs: `{"code", "url"}`) is left alone.

### Pool size

`postgres.pool.min` connections are opened at start and kept. With
`postgres.pool.max` above it, a query that finds no ready connection opens one
more — as many as there are queued queries not already covered by a connection
being opened, up to `max` — and a connection idle for
**`postgres.pool.idle_timeout`** seconds (default `60`, `0` — never) is closed
again while more than `min` are open; the pool checks it with its heartbeat,
once a minute, so a connection goes at the first check past the limit.
**`max` left out is `min`, and `max` equal to `min` keeps the pool at `min`.**
Before this, `max` had no effect and every pool stayed at `min`, so a
configuration that sets `max` above `min` now opens up to `max` under load:
count `max` × processes × copies against the server's (or the pooler's)
connection limit, or set `max` to `min`. A connection above `min` that the
server refuses (its connection limit) pauses growth for 4 s and does not move
the reconnect backoff of the pool's own connections. The pools the library
opens itself — a custom process's, `db_pool("helper")`, `db_pool("kernel")` —
take `min`/`max` from the settings; the worker's and the helper's take what the
application passes to `setup_db()` (the template passes the settings).

A custom process opens its pool (`postgres.helper`) lazily: no connection until
its first query, so a process that never runs one holds none — and a wrong
password or host shows in its log at that first query, not at start. Its LISTEN
connection is opened by `listen()` as before.

### Connection lifetime

A PostgreSQL server process does not give back what it has grown — the catalog
and plan caches of every function it has run — so a pool whose connections live
as long as the application keeps that memory for good. With
**`postgres.pool.lifetime`** seconds (default `0` — never) a pool connection
that has lived this long is replaced by a new one:

```json
{ "postgres": { "pool": { "min": 5, "max": 5, "lifetime": 3600 } } }
```

- A replacement starts only with the connection free (nothing in flight, no
  transaction block open), and the new connection is opened first. The old one
  goes on taking queries — last, after every other ready one — and is closed
  once the new one is up and it is itself free again. Count one connection more
  than the pool's size per pool for that while; a transaction block a caller
  opens on the old connection and leaves open stretches it until the block
  ends (an aborted block is closed at once — that is its ROLLBACK).
- A new connection that does not connect (a changed password, the server's
  connection limit), or is lost before the old one goes, leaves the old one in
  service, and the next attempt waits a minute. No connection is closed as idle
  (`idle_timeout`) while a replacement is in progress.
- The pool does not keep a caller's queries on one connection: a transaction
  block opened by one query and closed by another holds together only in a
  pool of one connection, and the replacement is a second one.
- One connection at a time, and only while the pool is healthy (no reconnect
  backoff pending, no connect in flight). Each connection's term is shortened
  by up to a tenth at random, so connections opened together do not all go
  together.
- Checked when a query completes and by the pool's heartbeat, once a minute:
  an idle connection goes at the first heartbeat past its term, the next idle
  one a minute later. A reconnect starts a new term; a connection being replaced
  that reconnects is still closed once its replacement is up.
- Every pool — worker, helper, custom process, `db_pool(role)`; not the LISTEN
  connection, not the leader's lock connection. Logged at notice:
  `Past postgres.pool.lifetime (…s): opening its successor`, then
  `Retired: past postgres.pool.lifetime (…s), its successor is up`.

### A time limit on connecting to PostgreSQL

The pool connects asynchronously, and libpq's asynchronous handshake keeps no
time: `connect_timeout` is honoured only by the blocking `PQconnectdb`. A server
that accepts the TCP connection and never answers — a hung host, a half-dead
proxy — used to hold a pool connection or the LISTEN connection in the
handshake for good. The pool keeps the limit itself:

- The limit is `connect_timeout`, whatever libpq will use: `postgres.timeout`
  in the config — on by default, the application's
  `APP_DEFAULT_PG_CONNECT_TIMEOUT` (5 s in the template) — or
  `PGCONNECT_TIMEOUT`, or a service file. Below 2 s it is 2 s, as in libpq. A
  connection string with no `connect_timeout`, or 0 — no limit, as in libpq.
- It counts per address, as libpq's does, but twice: once for the TCP
  connect and once more from the moment TCP is up — one address may take up
  to twice the limit. A TCP connect past the limit — a first address that
  drops SYN, an IPv6 black hole, a firewall that drops instead of rejecting —
  is given up, `Connect/Reset: TCP connect not finished in Ns, the address is
  given up` or `Listener: TCP connect not finished in Ns, …` in the PostgreSQL
  log, and libpq moves on to the next address or host, where the clock starts
  again; after the last one the connect fails as any failed connect, with
  libpq's text `Connection reset by peer`. Left to the kernel, such a connect
  took its SYN retries: about two minutes per address on Linux defaults.
- Past the limit (checked once a second, the verdict on the second check in a
  row) the connection is dropped as a failed connect —
  `Connect/Reset failed: handshake not finished in Ns` or
  `Listener Error: handshake not finished in Ns` in the PostgreSQL log — and
  reconnects with the usual backoff. The LISTEN channels are subscribed again
  once a server answers.
- With several hosts, a **first** host that accepts TCP and stays silent is
  not skipped: the asynchronous handshake cannot be moved on to the next host
  from outside, so every attempt starts from the first again.
  `load_balance_hosts=random` (libpq 16+) makes each attempt pick a host at
  random, so the pool gets through to a live one.

### A server that stops answering

A connection whose peer goes silent — a dropped route, a black hole after a
failover, no RST — is not broken as far as the client's kernel knows: a query
in flight waits until the kernel gives up retransmitting (`tcp_retries2`, about
15 minutes on Linux), and every pool slot it holds waits with it. Two settings,
both off by default:

- **TCP keys** — `keepalives_idle`, `keepalives_interval`, `keepalives_count`,
  `tcp_user_timeout`: libpq's own names and units, in `postgres.<key>` for every
  role or `postgres.<role>.<key>` for one. They are added to the connection
  string the application builds and to an explicit `postgres.<role>.conninfo`
  that does not set them itself (as query parameters for a
  `postgresql://` URI). A value that is not a whole number refuses the file.
  This is the detector: the kernel drops the connection, libpq reports
  `Connection timed out`, the query is failed as lost and the pool reconnects.
  A long statement on a live server is not affected — the server's kernel
  acknowledges. Example: `"tcp_user_timeout": 10000, "keepalives_idle": 10,
  "keepalives_interval": 5, "keepalives_count": 3`.
- **`postgres.stall_timeout`** (seconds, `0` — off) — how long a query in
  flight may go without a byte from the server. Past it the connection is
  dropped **without** `PQcancel` (a cancel to a
  dead address blocks the loop); its query is failed as lost —
  `Query in flight: no answer from the server in Ns (postgres.stall_timeout),
  dropping the connection without cancel` in the application's PostgreSQL log
  (`log.postgres`) — or re-queued if it was not sent whole or may be repeated,
  and the connection reconnects in its own slot. The LISTEN connection is not
  watched. A statement that runs longer than the limit without sending
  anything is dropped too: it is a safety net above the longest statement the
  application runs, not a statement timeout.

A failed connect moves the backoff (1, 2, then 4 s on) one step per round of
attempts, not per connection: a pool and its listener that meet one dead
server together count it once. A server back after a long outage is tried
within 4 s.

### Stopping without dropping the work in flight

On `SIGTERM` the master sends `SIGTERM` to its children and `SIGKILL` to those
still running after `process.stop_timeout` seconds (default `5`; `1`–`3600`,
checked with the file). A child, on `SIGTERM`, `SIGQUIT` and on the `SIGQUIT` of a
reload:

1. stops taking work: the listening socket leaves its loop (the master closes
   its own copy as well, so a new connection is refused at once once every
   stopping process has let go of it), idle keep-alive connections are closed,
   every open one closes after its next answer, which carries
   `Connection: close`; a WebSocket upgrade gets `503`. The modules are told by
   `on_stopping()` — take nothing new, finish what is started — and their
   `heartbeat()` is no longer called;
2. runs its loop until nothing is in flight — no query of a pool outstanding, no
   answer owed on an HTTP connection, no module reporting `busy()` — or until
   `stop_timeout` less 3.5 s runs out (never under 1.5 s); requests on the
   connections still open are served meanwhile. Every 1.5 s (a quarter of
   that time, or of `stop_timeout`, if less) a query with nowhere to run — no
   connection of its pool is up or connecting — is looked at: the first such
   round reconnects past the backoff, or waits, once, for the reconnect due
   before the next round; a later one fails the query once that attempt failed too
   (`not sent: no connection to the database, …`), so that a stop during an
   outage does not wait out `stop_timeout` and a request behind such a query
   gets an answer;
3. then, as before: modules latched, pools detached, `on_stop()`, a last drain
   of what `on_stop()` queued.

At the default 5 s the work in flight gets 1.5 s, as it always has. Where the
platform waits longer before its own kill — a container's stop grace period,
`terminationGracePeriodSeconds` — set `stop_timeout` a little under that and
above the longest piece of work a module may have started (an outbound call, a
send). A process that did not finish in time logs what it was waiting for:
`work in flight did not finish in N ms (queries: …, HTTP answers: …, busy
modules: …)`.

The wait is only as good as what it waits for: a deferred answer a module never
sends holds the stop to its end. Under 5 s the floor of 1.5 s and the last drain
can outlast the master's `SIGKILL`. A reload (`SIGHUP`) during a stop is ignored.

A module counts its own work in `busy()` — an outbound HTTP call, an SMTP send,
a step on a timer; queries and HTTP answers are counted by the framework. The
same pair, `on_stopping()` and `busy()`, is on `ProcessModule` and
`CustomProcess`.

## Build (standalone development)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_POSTGRESQL=ON -DWITH_SSL=ON -DWITH_CURL=ON
cmake --build build --parallel
```

Minimal build (no optional dependencies):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_POSTGRESQL=OFF -DWITH_SSL=OFF -DWITH_CURL=OFF
cmake --build build --parallel
```

## Directory Structure

```
include/apostol/        public headers (29 .hpp files)
src/
├── config/             JSON configuration with hot-reload
├── core/               event loop, process model, utilities, clients
├── log/                logging with gzip rotation
├── net/                TCP, HTTP, WebSocket, UDP
├── pg/                 PostgreSQL async pool
└── lib/                header-only deps (jwt-cpp, picojson)
CMakeLists.txt          build configuration
```

## Documentation

- **[Wiki](https://github.com/apostoldevel/libapostol/wiki)** — Getting Started, Architecture, Configuration, Creating Modules, Creating Processes

## Requirements

- **C++20** — GCC 12+ or Clang 16+
- **Linux** (epoll)
- **CMake** 3.25+
- **zlib** (required)
- OpenSSL, libpq, libcurl — optional (controlled by feature flags)

## License

[MIT](https://github.com/apostoldevel/libapostol/blob/master/LICENSE)

---

[^crm]: **Apostol CRM** — a template project built on the [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) and [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform) frameworks.
