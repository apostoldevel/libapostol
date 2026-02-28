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

## Возможности

| Категория  | Компоненты                                                                                     |
|------------|------------------------------------------------------------------------------------------------|
| Ядро       | Logger (gzip-ротация), Config (JSON, hot-reload, env), EventLoop (epoll+timerfd+signalfd), Process model (master/worker/helper/single), Crash handler (backtrace, addr2line), Settings |
| Сеть       | TCP (TLS через OpenSSL), HTTP/1.1 (llhttp, keep-alive, chunked), WebSocket (RFC 6455), UDP     |
| Клиенты    | FetchClient (CurlClient/HttpClient), SmtpClient, TcpClient, UdpClient, HttpProxy               |
| PostgreSQL | Async pool (libpq+epoll), LISTEN/NOTIFY, deferred dispatch (`exec_sql`), PG utils               |
| Безопасность | JWT-верификация (HS/RS/ES/PS через jwt-cpp), OAuth2-провайдеры, BotSession                    |
| Утилиты    | Base64 (RFC 4648), File utils (SHA256, MIME), HTTP utils, SiteConfig                            |

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

## Требования

- **C++20** — GCC 12+ или Clang 16+
- **Linux** (epoll)
- **CMake** 3.25+
- **zlib** (обязательно)
- OpenSSL, libpq, libcurl — опционально (управляется feature flags)

## Лицензия

[MIT](https://github.com/apostoldevel/libapostol/blob/master/LICENSE)
