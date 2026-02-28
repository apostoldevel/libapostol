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

## Features

| Category   | Components                                                                                     |
|------------|------------------------------------------------------------------------------------------------|
| Core       | Logger (gzip rotation), Config (JSON, hot-reload, env), EventLoop (epoll+timerfd+signalfd), Process model (master/worker/helper/single), Crash handler (backtrace, addr2line), Settings |
| Networking | TCP (TLS via OpenSSL), HTTP/1.1 (llhttp, keep-alive, chunked), WebSocket (RFC 6455), UDP      |
| Clients    | FetchClient (CurlClient/HttpClient), SmtpClient, TcpClient, UdpClient, HttpProxy              |
| PostgreSQL | Async pool (libpq+epoll), LISTEN/NOTIFY, deferred dispatch (`exec_sql`), PG utils              |
| Security   | JWT verification (HS/RS/ES/PS via jwt-cpp), OAuth2 providers, BotSession                       |
| Utilities  | Base64 (RFC 4648), File utils (SHA256, MIME), HTTP utils, SiteConfig                           |

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
| `WITH_POSTGRESQL` | `ON`    | PgPool, pg_utils, pg_exec, BotSession              |
| `WITH_SSL`        | `ON`    | TLS, JWT verification, SmtpClient                  |
| `WITH_CURL`       | `ON`    | FetchClient uses CurlClient (async libcurl)        |

All three flags can be turned `OFF` for a minimal build with no external
optional dependencies.

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

## Requirements

- **C++20** — GCC 12+ or Clang 16+
- **Linux** (epoll)
- **CMake** 3.25+
- **zlib** (required)
- OpenSSL, libpq, libcurl — optional (controlled by feature flags)

## License

[MIT](https://github.com/apostoldevel/libapostol/blob/master/LICENSE)
