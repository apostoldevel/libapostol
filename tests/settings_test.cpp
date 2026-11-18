// settings_test — AppSettings defaults and populate(), in this process, no
// files:
//
//   1. the PostgreSQL log (log.postgres) has no default: a fresh AppSettings,
//      a configuration without the key and a configuration that moves the
//      prefix all leave it empty — no file is opened unless one is named;
//   2. a named log.postgres is resolved as before: relative against the
//      prefix (the one the same configuration sets, too), absolute as given;
//      an empty string or null leaves it empty;
//   3. the other paths keep their build defaults (control): error, access and
//      stream logs, pid and lock files follow the prefix.
//
// Usage: settings_test

#include "apostol/config.hpp"
#include "apostol/settings.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace apostol;
namespace fs = std::filesystem;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("settings_test: FAIL %s\n", what.c_str());
    }
}

AppSettings populated(const char* json)
{
    AppSettings s;
    s.populate(Config::from_string(json));
    return s;
}

void expect_postgres_log(const AppSettings& s, const fs::path& expected, const std::string& what)
{
    check(s.postgres_log == expected,
          what + ": postgres_log = \"" + s.postgres_log.string() + "\", expected \"" + expected.string() + "\"");
}

// ── 1. No default ────────────────────────────────────────────────────────────

void test_no_default()
{
    expect_postgres_log(AppSettings{}, {}, "a fresh AppSettings");
    expect_postgres_log(populated("{}"), {}, "an empty configuration");
    expect_postgres_log(populated(R"({"log": {"level": "debug", "file": "logs/e.log"}})"), {},
                        "a log section without the key");
    expect_postgres_log(populated(R"({"prefix": "/srv/app/"})"), {}, "a moved prefix");
}

// ── 2. A named log ───────────────────────────────────────────────────────────

void test_named()
{
    const AppSettings base;

    expect_postgres_log(populated(R"({"log": {"postgres": "logs/pg.log"}})"),
                        fs::path(base.prefix) / "logs/pg.log", "a relative name");
    expect_postgres_log(populated(R"({"prefix": "/srv/app/", "log": {"postgres": "logs/pg.log"}})"),
                        "/srv/app/logs/pg.log", "a relative name under a moved prefix");
    expect_postgres_log(populated(R"({"log": {"postgres": "/var/log/pg.log"}})"),
                        "/var/log/pg.log", "an absolute name");
    expect_postgres_log(populated(R"({"log": {"postgres": ""}})"), {}, "an empty name");
    expect_postgres_log(populated(R"({"log": {"postgres": null}})"), {}, "a null name");
}

// ── 3. The other paths (control) ─────────────────────────────────────────────

void test_other_paths()
{
    const AppSettings fresh;
    check(fresh.error_log == fresh.resolve(APP_ERROR_LOG_FILE), "a fresh error_log is the build default");
    check(fresh.access_log == fresh.resolve(APP_ACCESS_LOG_FILE), "a fresh access_log is the build default");
    check(fresh.stream_log == fresh.resolve(APP_STREAM_LOG_FILE), "a fresh stream_log is the build default");

    const auto moved = populated(R"({"prefix": "/srv/app/"})");
    check(moved.error_log == fs::path("/srv/app/") / APP_ERROR_LOG_FILE, "error_log follows the prefix");
    check(moved.access_log == fs::path("/srv/app/") / APP_ACCESS_LOG_FILE, "access_log follows the prefix");
    check(moved.stream_log == fs::path("/srv/app/") / APP_STREAM_LOG_FILE, "stream_log follows the prefix");
    check(moved.pid_file == fs::path("/srv/app/") / APP_PID_FILE, "pid_file follows the prefix");
    check(moved.lock_file == fs::path("/srv/app/") / APP_LOCK_FILE, "lock_file follows the prefix");
}

} // namespace

int main()
{
    test_no_default();
    test_named();
    test_other_paths();

    if (g_failed) {
        std::printf("settings_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("settings_test: OK\n");
    return 0;
}
