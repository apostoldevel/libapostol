// logger_test — FileTarget when the disk says no.
//
//   1. reopen() into a directory that takes no new file: nothing is thrown,
//      the lines go on into the file open so far, the other targets reopen
//      theirs, and once the directory takes files again the next line
//      recreates the log at its path.
//   2. A rotation whose compression fails (gzwrite refused, as on a full
//      disk): the backup stays uncompressed and whole, no archive is left.
//      With compression working, the backup is an archive of the lines.
//
// gzwrite() and gzclose() of the library come from here and fail on demand;
// the real ones do the work otherwise.
//
// Exit: 0 pass, 1 fail, 77 skipped (run as root: permissions do not hold).

#include "apostol/logger.hpp"

#include <fmt/format.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace apostol;

static bool g_fail_gz = false;

extern "C" int gzwrite(gzFile file, voidpc buf, unsigned len)
{
    using Fn = int (*)(gzFile, voidpc, unsigned);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "gzwrite"));
    return g_fail_gz ? 0 : real(file, buf, len);
}

extern "C" int gzclose(gzFile file)
{
    using Fn = int (*)(gzFile);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "gzclose"));
    const int rc = real(file);
    return g_fail_gz ? Z_ERRNO : rc;
}

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("logger_test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const fs::path& p, const std::string& what)
{
    return read_file(p).find(what) != std::string::npos;
}

std::string gunzip(const fs::path& p)
{
    std::string out;
    gzFile gz = ::gzopen(p.c_str(), "rb");
    if (!gz)
        return out;
    char buf[4096];
    int n;
    while ((n = ::gzread(gz, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::gzclose(gz);
    return out;
}

void reopen_refused(const fs::path& dir)
{
    const auto a = dir / "a", b = dir / "b";
    fs::create_directories(a);
    fs::create_directories(b);

    Logger lg;
    lg.set_level(LogLevel::notice);
    lg.add_target(std::make_unique<FileTarget>((a / "a.log").string(), 0, 5, false));
    lg.add_target(std::make_unique<FileTarget>((b / "b.log").string(), 0, 5, false));
    lg.notice("one");

    // logrotate moved both files away; the first directory takes no new file.
    fs::rename(a / "a.log", a / "a.log.old");
    fs::rename(b / "b.log", b / "b.log.old");
    ::chmod(a.c_str(), 0555);

    bool threw = false;
    try {
        lg.reopen();
    } catch (const std::exception& e) {
        threw = true;
        std::printf("logger_test: reopen threw: %s\n", e.what());
    }
    check(!threw, "reopen() into a directory that takes no file does not throw");
    check(fs::exists(b / "b.log"), "the next target reopens its file");

    lg.notice("two");
    check(has(a / "a.log.old", "two"), "the refused target writes on into the file it has");
    check(has(a / "a.log.old", "cannot reopen log file"), "and notes there why it did not reopen");
    check(has(b / "b.log", "two"), "the reopened target writes into the new file");

    ::chmod(a.c_str(), 0755);
    lg.notice("three");
    check(has(a / "a.log", "three"), "once the directory takes files, the log is recreated at its path");
}

void compression_refused(const fs::path& dir)
{
    fs::create_directories(dir);
    const auto log = dir / "r.log";
    const std::string line(60, 'x');

    Logger lg;
    lg.set_level(LogLevel::notice);
    lg.add_target(std::make_unique<FileTarget>(log.string(), 4096, 5, true));

    // The first rotation: compression refused.
    g_fail_gz = true;
    for (int i = 0; i < 50; ++i)
        lg.notice("first {} {}", i, line);
    g_fail_gz = false;

    const auto plain = fs::path(log.string() + ".1");
    const auto gz    = fs::path(log.string() + ".1.gz");
    check(fs::exists(plain), "a failed compression keeps the backup uncompressed");
    check(has(plain, "first 0 ") && has(plain, "first 30 "), "and whole");
    check(!fs::exists(gz), "no archive is left of it");

    // The next rotation: compression works.
    for (int i = 0; i < 50; ++i)
        lg.notice("second {} {}", i, line);
    const auto unpacked = gunzip(gz);
    check(unpacked.find("second 0 ") != std::string::npos, "a working compression leaves an archive of the lines");
}

} // namespace

int main(int argc, char* argv[])
{
    if (::geteuid() == 0) {
        std::printf("logger_test: run as root — permissions do not hold, skipped\n");
        return 77;
    }
    const fs::path dir = fs::absolute(argc > 1 ? argv[1] : "logger_test.d");
    std::error_code ec;
    fs::permissions(dir / "reopen/a", fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);

    reopen_refused(dir / "reopen");
    compression_refused(dir / "rotate");

    fs::permissions(dir / "reopen/a", fs::perms::owner_all, fs::perm_options::add, ec);
    std::printf("logger_test: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
