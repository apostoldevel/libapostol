#pragma once

#include "apostol/config.hpp"
#include "apostol/fetch_policy.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace apostol
{

// ─── ValidationError ─────────────────────────────────────────────────────────

struct ValidationError
{
    std::string key;     // config key path, e.g. "server.port"
    std::string message; // human-readable description
};

// ─── AppSettings ─────────────────────────────────────────────────────────────
//
// Typed, validated application settings. All fields have compile-time defaults
// sourced from CMakeLists.txt target_compile_definitions().
//
// Usage:
//   AppSettings s;               // all fields = CMake defaults
//   s.populate(config);          // override from JSON config
//   auto errs = s.validate();    // strict validation (nginx-style)
//   if (!errs.empty()) { ... }
//
struct AppSettings
{
    // ── Application identity ──────────────────────────────────────────────
    std::string name        {APP_NAME};
    std::string description {APP_DESCRIPTION};
    std::string version     {APP_VERSION};
    std::string locale      {APP_DEFAULT_LOCALE};

    // ── Paths & prefixes ──────────────────────────────────────────────────
    // All relative paths are resolved relative to prefix.
    std::string prefix      {APP_PREFIX};       // e.g. /etc/apostol/
    std::string conf_prefix {APP_CONF_PREFIX};  // e.g. conf/

    std::filesystem::path conf_file;   // resolved: prefix + APP_CONF_FILE (conf_prefix is NOT applied — 30789c7)
    std::filesystem::path pid_file;    // resolved: prefix + APP_PID_FILE
    std::filesystem::path lock_file;   // resolved: prefix + APP_LOCK_FILE
    std::filesystem::path error_log;    // resolved: prefix + APP_ERROR_LOG_FILE
    std::filesystem::path access_log;   // resolved: prefix + APP_ACCESS_LOG_FILE
    std::filesystem::path stream_log;   // resolved: prefix + APP_STREAM_LOG_FILE
    std::filesystem::path postgres_log; // resolved: prefix + APP_POSTGRES_LOG_FILE

    // ── Logging ───────────────────────────────────────────────────────────
    std::string   log_level       {APP_DEFAULT_LOG_LEVEL};
    std::uint64_t log_max_size    {APP_DEFAULT_LOG_MAX_SIZE};
    int           log_keep_rotated{5};
    bool          log_compress    {true};

    // ── Server ────────────────────────────────────────────────────────────
    std::string   server_listen   {APP_DEFAULT_LISTEN};
    std::uint16_t server_port     {APP_DEFAULT_PORT};
    int           server_backlog  {APP_DEFAULT_LISTEN_BACKLOG};
    int           server_timeout  {APP_DEFAULT_SERVER_TIMEOUT}; // ms; 0 = infinite

    // ── Document & cache roots ────────────────────────────────────────────
    std::filesystem::path doc_root;     // resolved: prefix + APP_DOC_ROOT
    std::filesystem::path cache_prefix; // resolved: prefix + APP_CACHE_PREFIX

    // ── Process model ─────────────────────────────────────────────────────
    int  workers    {APP_DEFAULT_WORKERS};
    bool master     {false};
    bool helper     {false};
    bool daemon     {false};

    // One active copy per background role (helper, each custom process):
    // "process.leader" — the default for all of them, "module.<Name>.leader" —
    // per custom process. See PgLeaderLock and Application::run_as_leader().
    bool leader          {false};
    int  leader_interval {5};   // seconds between asking (standby) and probing (leader)

    // "process.stop_timeout" — seconds from SIGTERM to the master's SIGKILL of
    // the children still running. A child spends it finishing the work in flight
    // (Application::stop_work); on a deployment that waits longer before its own
    // kill (a container's stop grace period), set it a little under that.
    int  stop_timeout    {5};

    // ── Identity ──────────────────────────────────────────────────────────
    // "node" — this copy's name for node_id(); empty → $NODE_NAME → host name.
    std::string node;

    // ── System ────────────────────────────────────────────────────────────
    std::string   user         {APP_DEFAULT_USER};
    std::string   group        {APP_DEFAULT_GROUP};
    std::uint32_t limit_nofile {0};

    // ── PostgreSQL ────────────────────────────────────────────────────────
    bool        pg_connect         {false};
    bool        pg_notice          {false};
    int         pg_connect_timeout {APP_DEFAULT_PG_CONNECT_TIMEOUT}; // seconds; 0 = infinite
    std::string pg_conninfo_worker;
    std::string pg_conninfo_helper;
    std::string pg_conninfo_kernel;
    int         pg_pool_min{1};
    int         pg_pool_max{5};
    // postgres.stall_timeout — seconds a query in flight may go without a byte
    // from the server before its connection is dropped (PgPool::set_stall_timeout);
    // 0 = never.
    int         pg_stall_timeout{0};
    // postgres.reset — custom session settings ("prefix.name") reset before
    // every pool query (PgPool::set_reset); "all" — RESET ALL; [] — none.
    std::vector<std::string> pg_reset{"current.area"};

    // ── Outbound fetch ────────────────────────────────────────────────────
    // fetch.deny_internal + fetch.allow (T970); see fetch_policy.hpp.
    FetchAddressPolicy fetch_policy;

    // ── Constructor: resolve default paths from prefix ────────────────────
    AppSettings();

    // ── Populate from loaded Config (JSON). ───────────────────────────────
    void populate(const Config& cfg);

    // ── Strict validation (nginx-style). ──────────────────────────────────
    std::vector<ValidationError> validate() const;

    // ── Path resolution: resolve relative path against prefix. ────────────
    std::filesystem::path resolve(std::string_view relative) const;

    // ── Effective worker count: workers if > 0, else nproc. ───────────────
    int effective_workers() const;

    // ── Build PG conninfo from JSON config + env vars ───────────────────
    // role: "worker" | "helper" | "kernel"
    // The TCP keys keepalives_idle, keepalives_interval, keepalives_count and
    // tcp_user_timeout (libpq's names and units) are taken from
    // postgres.<role>.<key>, else postgres.<key>, and added to the string —
    // to an explicit postgres.<role>.conninfo too, unless it sets the key
    // itself. Not configured — not added. Throws ConfigError for a value that
    // is not a whole number.
    static std::string build_pg_conninfo(
        const Config& cfg,
        std::string_view role,
        int connect_timeout);
};

} // namespace apostol
