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
| Безопасность | JWT-верификация (HS/RS/ES/PS через jwt-cpp), OAuth2-провайдеры, BotSession, ServiceToken, CORS               |
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
| `WITH_POSTGRESQL` | `ON`         | PgPool, pg_utils, pg_exec, BotSession, ServiceToken |
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

### Конфигурация из окружения

Любое строковое значение JSON-конфигурации может ссылаться на переменную
окружения — `"${VAR}"` или `"$VAR"`; подстановка выполняется при каждой загрузке
файла, включая перечитывание по SIGHUP. Незаданная переменная подставляется
пустой строкой.
Подстановка работает только в строках, поэтому флаг или число, которое решает
площадка, пишется строкой, а `Config::get_bool` / `Config::get_int` её читают:

```json
{
  "module":   { "GatewayAPI": { "enable": "${GATEWAY_ENABLE}" } },
  "server":   { "port": "${PORT}" },
  "database": { "password": "${DB_PASSWORD}" }
}
```

`get_bool` принимает `true|false|1|0|yes|no|on|off` без учёта регистра;
`get_int` — целое десятичное число целиком. Пустая строка или одни пробелы
(переменная не задана) считается отсутствующим ключом: вариант с умолчанием
возвращает умолчание, обязательный бросает `ConfigError`. Любая другая строка —
`ConfigError` с именем ключа и значением, ничего не угадывается
(`GATEWAY_ENABLE=maybe` → `config key 'module.GatewayAPI.enable' must be a bool
(…), got string 'maybe'`). Куда это попадёт, зависит от того, кто читает ключ:
собственные настройки мастера — отказ старта; флаг модуля читается при старте
рабочего процесса, тот выходит, а мастер перезапускает его с задержкой — строка
ошибки повторяется, пока переменную не исправят.

### Одна активная копия на фоновую роль

Две копии приложения на одной базе — два контейнера, два пода, накатка — запускают
каждую фоновую роль дважды: два планировщика берут одно задание, два PGFetch
шлют один запрос. `"leader": true` оставляет активной одну копию каждой роли,
остальные ждут:

```json
{
  "node":    "${NODE_NAME}",
  "process": { "helper": true, "leader": "${LEADER}", "leader_interval": 5 },
  "module":  { "LicenseServer": { "enable": true, "leader": false } }
}
```

- `process.leader` — для helper-процесса и каждого custom-процесса;
  `module.<Name>.leader` переопределяет его для одного custom-процесса. Worker'ов
  не касается. Оба ключа проверяются вместе с файлом: не булево значение или
  `true` без `postgres.helper` — отказ старта, перезагрузки и `-t`.
- До старта модулей процесс берёт `pg_try_advisory_lock` на выделенном соединении
  (настройки `postgres.helper`), ключ `"<приложение>/<роль>"`. Взял — стартует; нет —
  ждёт как `… process (standby)` с одним соединением и без пула, спрашивает раз в
  `leader_interval` секунд и пишет в журнал, кто держит роль (`application_name`
  держателя: `"<приложение> <роль> <узел>:<pid>"`).
- Соединение лока должно доходить до PostgreSQL в режиме сессии — напрямую или
  через пул pgbouncer в режиме `session`. При транзакционном пулинге сессионный лок
  ничего не гарантирует: активны могут быть две копии сразу, а лок может остаться в
  серверном соединении pgbouncer после смерти процесса и запереть роль для всех.
  Лидер сообщает о смене серверной сессии и отступает — это диагноз, а не защита.
- Потеря лока — обрыв соединения, нет ответа сервера за max(3 × interval, 15 с), лок
  пропал — останавливает модули и завершает процесс с кодом 1; master запускает
  новый, и тот ждёт как резерв. Сессия ставит `idle_session_timeout` (PostgreSQL 14+)
  в четыре таких срока: лидер, пропавший без закрытия сокета, освобождает роль
  примерно за минуту — уже после того, как отступил по своим часам.
- Работа лидера по таймеру (`heartbeat`) после остановки процесса ждёт нового
  подтверждения лока, а перед каждым тактом сокет лока спрашивается, не закрыл ли
  его сервер: сессия, убитая, пока процесс стоял, — потеря до такта, а не после. Работу, пришедшую по сокету — NOTIFY, прочитанный в одном
  пробуждении с потерей, — так не упорядочить: где дубль стоит денег, строку нужно
  ещё и захватить в базе.

`node` — имя этой копии, `Application::node_id()`: значение из конфига, иначе
`$NODE_NAME` (в Kubernetes — имя пода из downward API), иначе имя хоста;
`node_process_id()` добавляет `:<pid>`. Учитывается только строка: объект под
`"node"` (у cs — `{"code", "url"}`) принадлежит приложению и не трогается.

### Размер пула

`postgres.pool.min` соединений открываются при старте и держатся. Если
`postgres.pool.max` больше, запрос, не нашедший готового соединения, открывает
ещё одно — столько, сколько в очереди запросов, не покрытых соединениями,
которые уже открываются, но не больше `max`, — а соединение, простоявшее
**`postgres.pool.idle_timeout`** секунд (по умолчанию `60`, `0` — никогда),
закрывается, пока открыто больше `min`; пул проверяет это своим heartbeat раз в
минуту, так что соединение уходит на первой проверке после срока.
**`max` не задан — значит, равен `min`; `max`, равный `min`, держит пул на `min`.**
Раньше `max` ни на что не влиял и любой пул оставался на `min`, так что
конфигурация с `max` больше `min` теперь под нагрузкой открывает до `max`:
посчитайте `max` × процессы × копии против предела соединений сервера (или
пулера) либо приравняйте `max` к `min`. Соединение сверх `min`, которому сервер
отказал (предел соединений), приостанавливает рост на 4 с и не двигает отступ
переподключения собственных соединений пула. Пулы, которые библиотека открывает
сама, — пользовательского процесса, `db_pool("helper")`, `db_pool("kernel")` —
берут `min`/`max` из настроек; пулы воркера и helper — то, что приложение
передаёт в `setup_db()` (шаблон передаёт настройки).

Пользовательский процесс открывает свой пул (`postgres.helper`) лениво: ни
одного соединения до первого запроса, так что процесс, который запросов не
делает, не держит ни одного, — а неверный пароль или хост видны в его журнале
при этом первом запросе, а не при старте. Соединение LISTEN открывает
`listen()`, как и раньше.

### Срок жизни соединения

Серверный процесс PostgreSQL не отдаёт то, что нарастил, — кэши каталога и
планов каждой функции, которую выполнял, — поэтому пул, чьи соединения живут
столько же, сколько приложение, держит эту память всегда. С
**`postgres.pool.lifetime`** секунд (по умолчанию `0` — никогда) соединение
пула, прожившее столько, заменяется новым:

```json
{ "postgres": { "pool": { "min": 5, "max": 5, "lifetime": 3600 } } }
```

- Замена начинается только со свободного соединения (нет запроса в полёте, нет
  открытого блока транзакции), и новое соединение открывается первым. Старое
  продолжает брать запросы — последним, после всех остальных готовых — и
  закрывается, когда новое подключилось, а оно само снова свободно. На это
  время считайте на пул одно соединение сверх его размера; блок транзакции,
  открытый на старом соединении и не закрытый, растягивает это время до конца
  блока (прерванный блок закрывается сразу — это и есть его ROLLBACK).
- Новое соединение, которое не подключилось (сменили пароль, предел соединений
  сервера) или пропало раньше, чем ушло старое, оставляет старое в работе, а
  следующая попытка ждёт минуту. Пока идёт замена, ни одно соединение не
  закрывается как простаивающее (`idle_timeout`).
- Пул не держит запросы одного вызывающего на одном соединении: блок
  транзакции, открытый одним запросом и закрытый другим, держится вместе только
  в пуле из одного соединения, а замена — второе.
- По одному соединению, и только пока пул здоров (нет ожидания переподключения,
  нет подключения в полёте). Срок каждого соединения случайно укорачивается до
  десятой доли, чтобы открытые вместе не уходили вместе.
- Проверяется по завершении запроса и в heartbeat пула, раз в минуту:
  простаивающее соединение уходит на первом heartbeat после срока, следующее
  простаивающее — минутой позже. Переподключение начинает новый срок;
  заменяемое соединение, которое переподключилось, всё равно закрывается, когда
  замена подключилась.
- Все пулы — worker, helper, пользовательского процесса, `db_pool(role)`; не
  соединение LISTEN и не соединение лока лидера. В журнал на уровне notice:
  `Past postgres.pool.lifetime (…s): opening its successor`, затем
  `Retired: past postgres.pool.lifetime (…s), its successor is up`.

### Срок на подключение к PostgreSQL

Пул подключается асинхронно, а асинхронное рукопожатие libpq времени не считает:
`connect_timeout` соблюдает только блокирующий `PQconnectdb`. Сервер, который принял
TCP-соединение и молчит — зависший хост, полумёртвый прокси, — держал соединение пула
или LISTEN-соединение в рукопожатии бессрочно. Срок пул теперь держит сам:

- Срок — `connect_timeout` в том виде, в каком его возьмёт libpq: `postgres.timeout`
  в конфиге — включён по умолчанию, это `APP_DEFAULT_PG_CONNECT_TIMEOUT` приложения
  (5 с в шаблоне), — либо `PGCONNECT_TIMEOUT`, либо service-файл. Меньше 2 с — 2 с,
  как в libpq. В строке подключения нет `connect_timeout` или он 0 — срока нет, как
  в libpq.
- Отсчёт идёт с момента, когда TCP установлен, — на каждый адрес, как в libpq. Сам
  TCP-connect оставлен ядру: если он не удался — первый адрес глушит SYN, чёрная
  дыра IPv6, — libpq переходит к следующему адресу или хосту, и часы там
  запускаются заново.
- По истечении срока (проверка раз в секунду, вердикт — на второй проверке подряд)
  соединение сбрасывается как неудачное подключение — в журнале PostgreSQL
  `Connect/Reset failed: handshake not finished in Ns` или
  `Listener Error: handshake not finished in Ns` — и переподключается с обычным
  отступом. Каналы LISTEN подписываются заново, как только сервер ответит.
- При нескольких хостах **первый** хост, который принял TCP и молчит, не
  пропускается: асинхронное рукопожатие снаружи на следующий хост не перевести, и
  каждая попытка снова начинается с первого. `load_balance_hosts=random` (libpq 16+)
  заставляет каждую попытку выбирать хост случайно, и пул добирается до живого.

### Сервер, который перестал отвечать

Соединение, чей собеседник замолчал — пропал маршрут, «чёрная дыра» после
переключения, без RST, — для ядра клиента не разорвано: запрос в полёте ждёт,
пока ядро не бросит повторы (`tcp_retries2`, около 15 минут в Linux), и с ним
ждёт каждый занятый слот пула. Две настройки, обе по умолчанию выключены:

- **Ключи TCP** — `keepalives_idle`, `keepalives_interval`, `keepalives_count`,
  `tcp_user_timeout`: имена и единицы libpq, в `postgres.<ключ>` для всех ролей
  или `postgres.<роль>.<ключ>` для одной. Дописываются в строку подключения,
  которую строит приложение, и в явный `postgres.<роль>.conninfo`, если он сам
  их не задаёт (для URI `postgresql://` — параметрами запроса). Значение, не
  являющееся целым числом, отвергает файл. Это и есть детектор: ядро рвёт
  соединение, libpq сообщает `Connection timed out`, запрос считается
  потерянным, пул переподключается. Долгий запрос на живом сервере не
  задевается — ядро сервера подтверждает. Пример: `"tcp_user_timeout": 10000,
  "keepalives_idle": 10, "keepalives_interval": 5, "keepalives_count": 3`.
- **`postgres.stall_timeout`** (секунды, `0` — выключено) — сколько запрос в
  полёте может не получать от сервера ни байта. Дальше соединение
  закрывается **без** `PQcancel` (отмена на мёртвый адрес блокирует
  цикл); запрос считается потерянным — `Query in flight: no answer from the
  server in Ns (postgres.stall_timeout), dropping the connection without
  cancel` в журнале PostgreSQL приложения (`log.postgres`) — или ставится в
  очередь снова, если ушёл не целиком или его можно повторять; соединение
  переподключается в своём слоте. Соединение LISTEN не наблюдается. Запрос,
  который законно молчит дольше срока, тоже будет оборван: это страховка выше
  самого долгого запроса приложения, а не statement timeout.

Неудачное подключение двигает отступ (1, 2, дальше 4 с) на шаг за круг попыток, а
не за соединение: пул и его слушатель, встретившие один мёртвый сервер вместе,
считают его один раз. К серверу, вернувшемуся после долгого сбоя, пул
обращается не позже чем через 4 с.

### Остановка без потери начатого

По `SIGTERM` master шлёт детям `SIGTERM`, а тем, кто ещё жив через
`process.stop_timeout` секунд (по умолчанию `5`; `1`–`3600`, проверяется вместе с
файлом), — `SIGKILL`. Ребёнок по `SIGTERM`, `SIGQUIT` и по `SIGQUIT` перезагрузки:

1. перестаёт брать работу: слушающий сокет уходит из его цикла (master закрывает
   и свою копию, так что новое соединение получает отказ сразу, как только все
   останавливающиеся процессы его отпустили), простаивающие keep-alive
   соединения закрываются, каждое открытое закрывается после следующего ответа,
   и этот ответ несёт `Connection: close`; на переход на WebSocket — `503`.
   Модулям сообщает `on_stopping()` — нового не брать, начатое доделать, — и
   `heartbeat()` больше не вызывается;
2. крутит цикл, пока что-то в полёте — запрос пула, ответ, который должен
   HTTP-соединению, модуль с `busy()` — или пока не истечёт `stop_timeout`
   минус 3,5 с (не меньше 1,5 с); запросы на ещё открытых соединениях тем
   временем обслуживаются. Каждые 1,5 с (четверть этого срока или `stop_timeout`,
   если она меньше) проверяется запрос, которому негде выполниться, — ни одно
   соединение его пула не живо и не подключается: первый такой раунд
   переподключается в обход отступа или ждёт — один раз — переподключения,
   назначенного до следующего раунда; один из следующих завершает запрос ошибкой, когда и эта
   попытка не удалась (`not sent: no connection to the database, …`):
   остановка во время сбоя не ждёт весь `stop_timeout`, и запрос, стоящий за
   ним, получает ответ;
3. дальше, как прежде: модули защёлкнуты, пулы отсоединены, `on_stop()`,
   последний слив того, что поставил `on_stop()`.

При умолчании 5 с начатому даётся 1,5 с, как и раньше. Где платформа ждёт
дольше перед своим убийством — срок остановки контейнера,
`terminationGracePeriodSeconds`, — ставьте `stop_timeout` чуть меньше его и
больше самой долгой работы, которую модуль мог начать (исходящий вызов,
отправка). Процесс, не успевший доделать, пишет, чего ждал: `work in flight did
not finish in N ms (queries: …, HTTP answers: …, busy modules: …)`.

Ожидание не лучше того, чего оно ждёт: отложенный ответ, который модуль не
пошлёт никогда, держит остановку до конца срока. При сроке меньше 5 с минимум
1,5 с и последний слив могут не успеть до `SIGKILL` master'а. Перезагрузка
(`SIGHUP`) во время остановки игнорируется.

Свою работу модуль учитывает в `busy()` — исходящий HTTP-вызов, отправку SMTP,
шаг по таймеру; запросы и HTTP-ответы считает каркас. Та же пара,
`on_stopping()` и `busy()`, есть у `ProcessModule` и `CustomProcess`.

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
