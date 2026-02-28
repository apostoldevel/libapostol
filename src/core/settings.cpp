#include "apostol/settings.hpp"

#include <fmt/format.h>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace apostol
{

// ─── Constructor ─────────────────────────────────────────────────────────────

AppSettings::AppSettings()
{
    conf_file    = resolve(std::string(APP_CONF_PREFIX) + APP_CONF_FILE);
    pid_file     = resolve(APP_PID_FILE);
    lock_file    = resolve(APP_LOCK_FILE);
    error_log    = resolve(APP_ERROR_LOG_FILE);
    access_log   = resolve(APP_ACCESS_LOG_FILE);
    stream_log   = resolve(APP_STREAM_LOG_FILE);
    postgres_log = resolve(APP_POSTGRES_LOG_FILE);
    doc_root     = resolve(APP_DOC_ROOT);
    cache_prefix = resolve(APP_CACHE_PREFIX);
}

// ─── Path resolution ─────────────────────────────────────────────────────────

std::filesystem::path AppSettings::resolve(std::string_view relative) const
{
    if (relative.empty())
        return {};
    if (relative[0] == '/')
        return std::filesystem::path(relative);

    std::string base = prefix;
    if (!base.empty() && base.back() != '/')
        base += '/';
    return std::filesystem::path(base + std::string(relative));
}

// ─── populate() ──────────────────────────────────────────────────────────────

void AppSettings::populate(const Config& cfg)
{
    locale = cfg.get_string("locale", locale);

    auto new_prefix = cfg.get_string("prefix", prefix);
    if (new_prefix != prefix)
    {
        prefix = new_prefix;
        conf_file    = resolve(std::string(APP_CONF_PREFIX) + APP_CONF_FILE);
        pid_file     = resolve(APP_PID_FILE);
        lock_file    = resolve(APP_LOCK_FILE);
        error_log    = resolve(APP_ERROR_LOG_FILE);
        access_log   = resolve(APP_ACCESS_LOG_FILE);
        stream_log   = resolve(APP_STREAM_LOG_FILE);
        postgres_log = resolve(APP_POSTGRES_LOG_FILE);
        doc_root     = resolve(APP_DOC_ROOT);
        cache_prefix = resolve(APP_CACHE_PREFIX);
    }

    if (auto v = cfg.get_string_opt("daemon.pid"))
        pid_file = resolve(*v);
    if (auto v = cfg.get_string_opt("daemon.lock"))
        lock_file = resolve(*v);

    log_level = cfg.get_string("log.level", log_level);
    if (auto v = cfg.get_string_opt("log.file"))
        error_log = resolve(*v);
    if (auto v = cfg.get_string_opt("log.access"))
        access_log = resolve(*v);
    if (auto v = cfg.get_string_opt("log.stream"))
        stream_log = resolve(*v);
    if (auto v = cfg.get_string_opt("log.postgres"))
        postgres_log = resolve(*v);
    log_max_size = static_cast<std::uint64_t>(
        cfg.get_int("log.max_size", static_cast<std::int64_t>(log_max_size)));
    log_keep_rotated = static_cast<int>(
        cfg.get_int("log.keep_rotated", log_keep_rotated));
    log_compress = cfg.get_bool("log.compress", log_compress);

    server_listen   = cfg.get_string("server.listen",   server_listen);
    server_port     = static_cast<std::uint16_t>(cfg.get_int("server.port",    server_port));
    server_backlog  = static_cast<int>(cfg.get_int("server.backlog",  server_backlog));
    server_timeout  = static_cast<int>(cfg.get_int("server.timeout",  server_timeout));

    if (auto v = cfg.get_string_opt("server.root"))
        doc_root = resolve(*v);
    if (auto v = cfg.get_string_opt("cache.prefix"))
        cache_prefix = resolve(*v);

    workers    = static_cast<int>(cfg.get_int("workers", workers));
    master     = cfg.get_bool("process.master",  master);
    helper     = cfg.get_bool("process.helper",  helper);
    daemon     = cfg.get_bool("daemon.enabled",  daemon);

    user         = cfg.get_string("process.user",         user);
    group        = cfg.get_string("process.group",        group);
    limit_nofile = static_cast<std::uint32_t>(
        cfg.get_int("process.limit_nofile", limit_nofile));

    pg_connect         = cfg.get_bool("postgres.connect",          pg_connect);
    pg_notice          = cfg.get_bool("postgres.notice",           pg_notice);
    pg_connect_timeout = static_cast<int>(cfg.get_int("postgres.timeout", pg_connect_timeout));

    pg_conninfo_worker = build_pg_conninfo(cfg, "worker", pg_connect_timeout);
    pg_conninfo_helper = build_pg_conninfo(cfg, "helper", pg_connect_timeout);
    pg_conninfo_kernel = build_pg_conninfo(cfg, "kernel", pg_connect_timeout);

    // Fallback: helper ← worker
    if (pg_conninfo_helper.empty())
        pg_conninfo_helper = pg_conninfo_worker;

    // Fallback: kernel ← worker
    if (pg_conninfo_kernel.empty())
        pg_conninfo_kernel = pg_conninfo_worker;

    pg_pool_min = static_cast<int>(cfg.get_int("postgres.pool.min", pg_pool_min));
    pg_pool_max = static_cast<int>(cfg.get_int("postgres.pool.max", pg_pool_max));
}

// ─── build_pg_conninfo() ─────────────────────────────────────────────────────

std::string AppSettings::build_pg_conninfo(
    const Config& cfg,
    std::string_view role,
    int connect_timeout)
{
    auto key = [&](std::string_view param) {
        return fmt::format("postgres.{}.{}", role, param);
    };

    // Read a JSON value as string, tolerating number/bool types (e.g. "port": 5432)
    auto get_as_string = [&](const std::string& k) -> std::string {
        const auto& j = cfg.json();
        // Navigate dot-separated path manually
        const nlohmann::json* node = &j;
        std::string_view path = k;
        while (!path.empty())
        {
            auto dot = path.find('.');
            auto segment = path.substr(0, dot);
            if (!node->is_object() || !node->contains(std::string(segment)))
                return {};
            node = &(*node)[std::string(segment)];
            path = (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
        }
        if (node->is_string())
            return node->get<std::string>();
        if (node->is_number_integer())
            return std::to_string(node->get<std::int64_t>());
        if (node->is_number_float())
            return std::to_string(node->get<double>());
        if (node->is_boolean())
            return node->get<bool>() ? "true" : "false";
        return {};
    };

    // 1. If explicit conninfo string is set, use as-is (backward compat)
    auto conninfo = get_as_string(key("conninfo"));
    if (!conninfo.empty())
        return conninfo;

    // 2. Read individual params from JSON config
    auto host     = get_as_string(key("host"));
    auto port     = get_as_string(key("port"));
    auto dbname   = get_as_string(key("dbname"));
    auto user     = get_as_string(key("user"));
    auto password  = get_as_string(key("password"));
    auto sslmode  = get_as_string(key("sslmode"));

    // Normalize boolean sslmode: false → "disable", true → "require"
    if (sslmode == "false") sslmode = "disable";
    else if (sslmode == "true") sslmode = "require";

    // 3. Apply base env var overrides (apply to all roles)
    auto env_or = [](const char* var, std::string& val) {
        if (auto* e = std::getenv(var); e && *e)
            val = e;
    };

    env_or("PGHOST",     host);
    env_or("PGHOSTADDR", host);  // PGHOSTADDR overrides PGHOST
    env_or("PGPORT",     port);
    env_or("PGDATABASE", dbname);
    env_or("PGUSER",     user);
    env_or("PGPASSWORD", password);
    env_or("PGSSLMODE",  sslmode);

    // 4. Apply role-specific env var overrides
    if (role == "helper")
    {
        env_or("PGHOSTAPI",     host);
        env_or("PGPORTAPI",     port);
        env_or("PGUSERAPI",     user);
        env_or("PGPASSWORDAPI", password);
    }
    else if (role == "kernel")
    {
        env_or("PGUSERKERNEL",     user);
        env_or("PGPASSWORDKERNEL", password);
    }

    // 5. Build conninfo string from non-empty params
    std::string result;
    auto append = [&](std::string_view param_name, const std::string& val) {
        if (val.empty())
            return;
        if (!result.empty())
            result += ' ';
        result += param_name;
        result += '=';
        result += val;
    };

    append("host",     host);
    append("port",     port);
    append("dbname",   dbname);
    append("user",     user);
    append("password", password);
    append("sslmode",  sslmode);

    if (connect_timeout > 0 && !result.empty())
        append("connect_timeout", std::to_string(connect_timeout));

    return result;
}

// ─── validate() ──────────────────────────────────────────────────────────────

std::vector<ValidationError> AppSettings::validate() const
{
    std::vector<ValidationError> errors;

    auto err = [&errors](std::string_view key, std::string msg) {
        errors.push_back({std::string(key), std::move(msg)});
    };

    // Log level
    static constexpr std::string_view valid_levels[] = {
        "debug", "info", "notice", "warn", "error", "crit", "alert", "emerg"};
    bool level_ok = false;
    for (auto& l : valid_levels)
        if (l == log_level) { level_ok = true; break; }
    if (!level_ok)
        err("log.level",
            fmt::format("invalid log level '{}'; must be one of: debug info notice warn error crit alert emerg",
                log_level));

    // Server port
    if (server_port == 0)
        err("server.port", "server port must be in range 1-65535 (0 is invalid)");

    // Server backlog
    if (server_backlog <= 0)
        err("server.backlog",
            fmt::format("server.backlog must be > 0, got {}", server_backlog));

    // Server timeout (0 = infinite — allowed)
    if (server_timeout < 0)
        err("server.timeout",
            fmt::format("server.timeout must be >= 0 ms, got {}", server_timeout));

    // Postgres connect timeout (0 = infinite — allowed)
    if (pg_connect_timeout < 0)
        err("postgres.timeout",
            fmt::format("postgres.timeout must be >= 0 s, got {}", pg_connect_timeout));

    // Workers
    if (workers < 0)
        err("workers",
            fmt::format("workers must be >= 0, got {}", workers));
    if (workers > 256)
        err("workers",
            fmt::format("workers must be <= 256, got {} (use 0 for auto)", workers));

    // Pool sizes
    if (pg_pool_min <= 0)
        err("postgres.pool.min",
            fmt::format("postgres.pool.min must be > 0, got {}", pg_pool_min));
    if (pg_pool_max < pg_pool_min)
        err("postgres.pool.max",
            fmt::format("postgres.pool.max ({}) must be >= pool.min ({})",
                pg_pool_max, pg_pool_min));

    // Prefix
    if (prefix.empty())
        err("prefix", "prefix path must not be empty");

    return errors;
}

// ─── effective_workers() ─────────────────────────────────────────────────────

int AppSettings::effective_workers() const
{
    if (workers > 0)
        return workers;
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    return (nproc > 0) ? static_cast<int>(nproc) : 1;
}

} // namespace apostol
