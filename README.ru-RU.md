[![en](https://img.shields.io/badge/lang-en-green.svg)](README.md)

# libapostol (Апостол)

**Апостол** — высокопроизводительный C++20-фреймворк для разработки серверных
приложений и системных служб под Linux с прямым доступом к PostgreSQL.

> A-POST-OL → **A**synchronous **POST** **O**rchestration **L**oop — единый
> цикл обработки событий (event loop) для HTTP и PostgreSQL.

HTTP-сервер и сокеты PostgreSQL работают в **едином event loop**. Данные идут
напрямую между HTTP-сервером и базой данных — без промежуточных скриптовых
слоёв (PHP, Python и т.п.). Это минимизирует задержки, снижает накладные расходы
и обеспечивает предсказуемую производительность.

> **libapostol — это и есть сам фреймворк Apostol.**
> Репозиторий [apostol](https://github.com/apostoldevel/apostol) — это пример
> сборки (example assembly) из libapostol и переиспользуемых модулей.

---

## Быстрый старт

Создайте новый проект из шаблона libapostol:

```bash
bash <(curl -sL https://raw.githubusercontent.com/apostoldevel/libapostol/master/install.sh)
```

Сборка и запуск (примеры используют имя проекта `myapp` по умолчанию — если выбрали другое, подставьте его в командах):

```bash
cd myapp
./configure --debug
cmake --build cmake-build-debug --parallel $(nproc)

# Для локальной разработки: измените "prefix" в conf/default.json на "."
mkdir -p logs
./cmake-build-debug/myapp -p . -c conf/default.json

curl http://localhost:4977/api/v1/ping   # → {"ok":true,"message":"OK"}
curl http://localhost:4977/docs          # → Swagger UI
```

Подробное руководство: **[Getting Started](https://github.com/apostoldevel/libapostol/wiki/Getting-Started)** в wiki.

---

## REST API со встроенным Swagger UI

Наследуйте `RoutedModule`, определите маршруты — интерактивная документация API
генерируется автоматически. Без внешних инструментов, кодогенерации и ручного YAML.

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

Каждый маршрут, зарегистрированный через `RouteBuilder`, автоматически доступен как:

- **`/docs`** — интерактивный [Swagger UI](https://swagger.io/tools/swagger-ui/) (загружается из CDN, без статических файлов)
- **`/docs/api.json`** — спецификация OpenAPI 3.0 (JSON)
- **`/docs/api.yaml`** — спецификация OpenAPI 3.0 (YAML)

См. [Creating Modules](https://github.com/apostoldevel/libapostol/wiki/Creating-Modules) — полный справочник `RouteBuilder` API.

---

## Возможности

| Категория  | Компоненты                                                                                     |
|------------|------------------------------------------------------------------------------------------------|
| Ядро       | Logger (gzip-ротация), Config (JSON, hot-reload, env), EventLoop (epoll+timerfd+signalfd), Process model (master/worker/helper/single), Crash handler (backtrace, addr2line), Settings |
| Сеть       | TCP (TLS через OpenSSL), HTTP/1.1 (llhttp, keep-alive, chunked), WebSocket (RFC 6455), UDP     |
| Клиенты    | FetchClient (CurlClient/HttpClient), SmtpClient, TcpClient, UdpClient, HttpProxy               |
| PostgreSQL | Async pool (libpq+epoll), LISTEN/NOTIFY, deferred dispatch (`exec_sql`), PG utils               |
| REST API   | RoutedModule + RouteBuilder fluent API, параметры в путях, wildcard-маршруты, генерация спецификации OpenAPI 3.0, встроенный Swagger UI |
| Безопасность | JWT-верификация (HS/RS/ES/PS через jwt-cpp), OAuth2-провайдеры, BotSession, CORS               |
| Утилиты    | Base64 (RFC 4648), File utils (SHA256, MIME), HTTP utils, RouteManager, SiteConfig              |

## Технологический стек

| Библиотека          | Версия   | Тип             |
|---------------------|----------|-----------------|
| nlohmann/json       | v3.11.3  | FetchContent    |
| {fmt}               | v11.1.4  | FetchContent    |
| llhttp              | v9.2.1   | FetchContent    |
| OpenSSL             | system   | опционально     |
| libcurl             | system   | опционально     |
| libpq (PostgreSQL)  | system   | опционально     |
| zlib                | system   | обязательно     |

## Feature Flags

| Флаг              | По умолчанию | Что включает                                       |
|-------------------|--------------|----------------------------------------------------|
| `WITH_POSTGRESQL` | `ON`         | PgPool, pg_utils, pg_exec, BotSession              |
| `WITH_SSL`        | `ON`         | TLS, JWT-верификация, SmtpClient                   |
| `WITH_CURL`       | `ON`         | FetchClient использует CurlClient (async libcurl)  |

Все три флага можно выставить в `OFF` для минимальной сборки без опциональных
зависимостей.

---

## Шаблонные проекты

libapostol — это конструктор модулей. Выбираете нужные модули и собираете из них
приложение. Два референсных проекта демонстрируют этот подход:

### Apostol

**[apostol](https://github.com/apostoldevel/apostol)** — HTTP-шлюз к PostgreSQL.

| Модуль | Тип | Описание |
|--------|-----|----------|
| [PGHTTP](https://github.com/apostoldevel/module-PGHTTP) | Worker | HTTP → диспетчеризация PL/pgSQL-функций |
| [WebServer](https://github.com/apostoldevel/module-WebServer) | Worker | Раздача статических файлов + Swagger UI |
| [PGFetch](https://github.com/apostoldevel/module-PGFetch) | Helper | LISTEN/NOTIFY → исходящие HTTP-запросы |

### Apostol CRM

[Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm] — полноценная backend-платформа: авторизация, REST API, файловый сервер, WebSocket, фоновые процессы.

---

## Модули

Все модули самодостаточны и переиспользуемы между проектами:

### Workers (обработчики HTTP/WebSocket-запросов)

| Модуль | Описание | Требования |
|--------|----------|------------|
| [PGHTTP](https://github.com/apostoldevel/module-PGHTTP) | HTTP → диспетчеризация PL/pgSQL-функций | WITH_POSTGRESQL |
| [WebServer](https://github.com/apostoldevel/module-WebServer) | Раздача статических файлов + Swagger UI | — |
| [AuthServer](https://github.com/apostoldevel/module-AuthServer) | Сервер авторизации OAuth 2.0 + JWT | WITH_POSTGRESQL + WITH_SSL |
| [AppServer](https://github.com/apostoldevel/module-AppServer) | REST API с авторизацией → PostgreSQL | WITH_POSTGRESQL + WITH_SSL |
| [FileServer](https://github.com/apostoldevel/module-FileServer) | Файловый сервер с JWT-авторизацией | WITH_POSTGRESQL + WITH_SSL |
| [WebSocketAPI](https://github.com/apostoldevel/module-WebSocketAPI) | JSON-RPC + pub/sub через WebSocket | WITH_POSTGRESQL + WITH_SSL |
| [StreamServer](https://github.com/apostoldevel/process-StreamServer) | Обработка UDP-датаграмм через PG | WITH_POSTGRESQL |

### Helpers (фоновые модули в helper-процессе)

| Модуль | Описание | Требования |
|--------|----------|------------|
| [PGFetch](https://github.com/apostoldevel/module-PGFetch) | LISTEN/NOTIFY → исходящие HTTP-запросы | WITH_POSTGRESQL |
| [PGFile](https://github.com/apostoldevel/module-PGFile) | LISTEN/NOTIFY → синхронизация файлов (PG ↔ файловая система) | WITH_POSTGRESQL |

### Процессы (независимые фоновые процессы)

| Процесс | Описание | Требования |
|---------|----------|------------|
| [TaskScheduler](https://github.com/apostoldevel/process-TaskScheduler) | Выполнение задач по расписанию из очереди `db.job` | WITH_POSTGRESQL |
| [ReportServer](https://github.com/apostoldevel/process-ReportServer) | Генерация отчётов по LISTEN/NOTIFY | WITH_POSTGRESQL |
| [MessageServer](https://github.com/apostoldevel/process-MessageServer) | Рассылка сообщений (SMTP / FCM / HTTP API) | WITH_POSTGRESQL + WITH_SSL |
| [Replication](https://github.com/apostoldevel/process-Replication) | Синхронизация данных master-slave по HTTP | WITH_POSTGRESQL |

---

## Бенчмарк

**Apostol v2 vs v1 vs Python vs Node.js vs Go vs Nginx** — сравнительное
тестирование в идентичных Docker-условиях.

### /ping — без базы данных (keep-alive ON, 100 соединений)

| Сервис | RPS | Latency p50 |
|--------|----:|------------:|
| Nginx (static return) | 566,000 | 111us |
| **Apostol v2** | **507,000** | **170us** |
| Go (net/http) | 211,000 | 0.45ms |
| Apostol v1 | 128,000 | 790us |
| Node.js (Fastify) | 102,000 | 0.95ms |
| Python (FastAPI) | 2,400 | 41ms |

### /db/ping — PostgreSQL round-trip (keep-alive ON, 100 соединений)

| Сервис | RPS | Latency p50 |
|--------|----:|------------:|
| **Apostol v2** | **112,000** | **0.91ms** |
| Go | 72,000 | 1.07ms |
| Apostol v1 | 61,000 | 1.61ms |
| Node.js | 36,000 | 2.65ms |
| Python | 2,300 | 42ms |

**Ключевые результаты:**
- Apostol v2 в **4 раза быстрее** v1 и в **2.4 раза быстрее** Go на /ping
- Apostol v2 достигает **90% пропускной способности Nginx** на /ping с keep-alive (507K vs 566K) и **не уступает Nginx** без keep-alive (84K vs 84K при c100)
- При 1000 соединениях без keep-alive Apostol v2 **опережает Nginx** (81K vs 75K RPS) благодаря `SO_REUSEPORT`

> Полные результаты, методология и анализ:
> [REST API Benchmark](https://github.com/apostoldevel/apostol/blob/master/doc/BENCHMARK.ru-RU.md).

---

## Использование

Добавьте libapostol как подкаталог (например, git submodule) и слинкуйте:

```cmake
add_subdirectory(src/lib/libapostol)
target_link_libraries(my_app PRIVATE apostol)
```

Feature flags (`WITH_POSTGRESQL`, `WITH_SSL`, `WITH_CURL`) объявлены как
`PUBLIC` compile definitions — потребительские таргеты получают их автоматически.

## Сборка (standalone-разработка)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_POSTGRESQL=ON -DWITH_SSL=ON -DWITH_CURL=ON
cmake --build build --parallel
```

Минимальная сборка (без опциональных зависимостей):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_POSTGRESQL=OFF -DWITH_SSL=OFF -DWITH_CURL=OFF
cmake --build build --parallel
```

## Структура директорий

```
include/apostol/        публичные заголовки (29 файлов .hpp)
src/
├── config/             JSON-конфигурация с hot-reload
├── core/               event loop, process model, утилиты, клиенты
├── log/                логирование с gzip-ротацией
├── net/                TCP, HTTP, WebSocket, UDP
├── pg/                 асинхронный пул PostgreSQL
└── lib/                header-only зависимости (jwt-cpp, picojson)
CMakeLists.txt          конфигурация сборки
```

## Документация

- **[Wiki](https://github.com/apostoldevel/libapostol/wiki)** — Getting Started, Architecture, Configuration, Creating Modules, Creating Processes

## Требования

- **C++20** — GCC 12+ или Clang 16+
- **Linux** (epoll)
- **CMake** 3.25+
- **zlib** (обязательно)
- OpenSSL, libpq, libcurl — опционально (управляется feature flags)

## Лицензия

[MIT](https://github.com/apostoldevel/libapostol/blob/master/LICENSE)

---

[^crm]: **Apostol CRM** — шаблон-проект построенный на фреймворках [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) и [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform).
