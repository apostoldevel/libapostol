// settings_test — a number its setting cannot hold is a validation error,
// and the setting keeps its value; numbers in range pass as before.

#include "apostol/config.hpp"
#include "apostol/settings.hpp"

#include <cstdio>
#include <string>

using namespace apostol;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("settings_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

bool names(const std::vector<ValidationError>& errs, const std::string& key)
{
    for (const auto& e : errs)
        if (e.key == key)
            return true;
    return false;
}

} // namespace

int main()
{
    {
        AppSettings s;
        const AppSettings before;
        s.populate(Config::from_string(R"({"workers": 4294967297, "log": {"max_size": -1},
            "process": {"limit_nofile": -1}, "server": {"port": 65537},
            "postgres": {"pool": {"min": 4294967298}}})"));
        const auto errs = s.validate();
        for (const char* key : {"workers", "log.max_size", "process.limit_nofile", "server.port",
                                "postgres.pool.min"})
            check(names(errs, key), std::string("out of range: an error names ") + key);
        check(s.workers == before.workers, "workers keeps its value");
        check(s.log_max_size == before.log_max_size, "log.max_size keeps its value");
        check(s.limit_nofile == before.limit_nofile, "process.limit_nofile keeps its value");
    }
    {
        AppSettings s;
        s.populate(Config::from_string(R"({"workers": 3, "log": {"max_size": 1048576},
            "process": {"limit_nofile": 4096}, "server": {"port": 8080}})"));
        const auto errs = s.validate();
        check(!names(errs, "workers") && !names(errs, "log.max_size") && !names(errs, "server.port")
              && !names(errs, "process.limit_nofile"), "in range: no error");
        check(s.workers == 3 && s.log_max_size == 1048576 && s.limit_nofile == 4096 && s.server_port == 8080,
              "in range: the values are taken");
    }
    std::printf("settings_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
