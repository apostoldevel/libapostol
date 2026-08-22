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
