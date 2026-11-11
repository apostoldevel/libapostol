// path_under_root_test — path_under_root() and ApostolModule::try_files(), in
// this process, on a temporary tree (mkdtemp in the system temp directory):
//
//   1. a request path that stays under the root resolves to root/<path>, the
//      path part normalized lexically, the root used as given (with or without
//      a trailing '/', "/", empty, relative, holding ".."); "/" is the root;
//   2. an empty path, a path without a leading '/', a NUL, a path starting
//      with "//", and ".." segments leading out of the root resolve to an
//      empty path;
//   3. try_files serves a file, a directory index (301 without a trailing
//      '/'), and the fallback, as before; a relative root works the same;
//   4. try_files answers 400 to a path that does not stay under the root, and
//      the response holds nothing of a file outside it; a fallback that does
//      not stay under the root is skipped (404);
//   5. HEAD: every answer (200, 301, 400, 404) carries no body.
//
// Usage: path_under_root_test

#include "apostol/apostol_module.hpp"
#include "apostol/file_utils.hpp"
#include "apostol/http.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace apostol;
namespace fs = std::filesystem;

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failed;
        std::printf("path_under_root_test: FAIL %s\n", what.c_str());
    }
}

std::string shown(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        if (c == '\0') c = '0';
    return out;
}

void expect_path(const fs::path& root, std::string_view path, const std::string& expected)
{
    const auto got = path_under_root(root, path);
    check(got.string() == expected,
          "path_under_root(\"" + root.string() + "\", \"" + shown(path) + "\") = \"" + got.string()
              + "\", expected \"" + expected + "\"");
}

// ── 1, 2. The table ──────────────────────────────────────────────────────────

void test_table()
{
    using namespace std::string_view_literals;

    for (const fs::path& root : {fs::path("/srv/www"), fs::path("/srv/www/")}) {
        const auto under = [&](std::string_view rel) { return (root / rel).string(); };

        // Under the root.
        expect_path(root, "/a.txt", under("a.txt"));
        expect_path(root, "/", root.string());
        expect_path(root, "/./", root.string());
        expect_path(root, "/a/..", root.string());
        expect_path(root, "/d/", under("d/"));
        expect_path(root, "/d/a.txt", under("d/a.txt"));
        expect_path(root, "/a//b", under("a/b"));
        expect_path(root, "/a/./b", under("a/b"));
        expect_path(root, "/a/../b", under("b"));
        expect_path(root, "/..a", under("..a"));
        expect_path(root, "/.../x", under(".../x"));
        expect_path(root, "/%2Fetc", under("%2Fetc"));

        // Not under the root.
        expect_path(root, "", "");
        expect_path(root, "x", "");
        expect_path(root, "a/b", "");
        expect_path(root, "//etc/hostname", "");
        expect_path(root, "///x", "");
        expect_path(root, "//", "");
        expect_path(root, "/..", "");
        expect_path(root, "/../", "");
        expect_path(root, "/../x", "");
        expect_path(root, "/a/../../x", "");
        expect_path(root, "/../www/x", "");
        expect_path(root, "/../www2/x", "");
        expect_path(root, "/a\0b"sv, "");
    }

    // The root "/".
    expect_path("/", "/", "/");
    expect_path("/", "/etc/hostname", "/etc/hostname");
    expect_path("/", "/..", "");
    expect_path("/", "//etc/hostname", "");

    // An empty root is the working directory.
    expect_path("", "/", ".");
    expect_path("", "/a.txt", "./a.txt");
    expect_path("", "/../x", "");

    // A relative root.
    expect_path("www", "/a.txt", "www/a.txt");
    expect_path("www/", "/d/a.txt", "www/d/a.txt");
    expect_path("www", "/../x", "");
    expect_path("www", "//x", "");

    // The root is used as given, not normalized.
    expect_path("/srv/a/../b", "/x", "/srv/a/../b/x");
    expect_path("/srv/a/../b", "/../x", "");
}

// ── 3, 4, 5. try_files on a tree ─────────────────────────────────────────────

class Probe final : public ApostolModule
{
public:
    std::string_view name() const override { return "Probe"; }
    bool enabled() const override { return true; }

    HttpResponse get(const fs::path& root, const std::string& path, bool head_only = false,
                     const std::vector<std::string>& fallbacks = {"/index.html"})
    {
        HttpRequest req;
        req.method = head_only ? "HEAD" : "GET";
        req.path = path;
        req.version = "HTTP/1.1";
        HttpResponse resp;
        const bool sent = try_files(root, req, resp, head_only, fallbacks);
        check(sent, "try_files(\"" + path + "\") returned false");
        return resp;
    }

protected:
    void init_methods() override {}
};

bool write(const fs::path& p, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << data;
    return f.good();
}

constexpr std::string_view kOutside = "OUTSIDE-OF-THE-ROOT";

void expect(Probe& m, const fs::path& root, const std::string& path, int status,
            std::string_view has = {}, const std::vector<std::string>& fallbacks = {"/index.html"},
            bool head_only = false)
{
    const auto resp = m.get(root, path, head_only, fallbacks);
    const auto wire = resp.serialize();
    const std::string what = std::string(head_only ? "HEAD" : "GET") + " \"" + path + "\" from \""
                           + root.string() + "\"";
    check(resp.status_code() == status,
          what + ": status " + std::to_string(resp.status_code()) + ", expected " + std::to_string(status));
    if (!has.empty())
        check(wire.find(has) != std::string::npos, what + ": response lacks \"" + std::string(has) + "\"");
    check(wire.find(kOutside) == std::string::npos, what + ": response holds a file outside the root");
}

// The response has a header block and nothing after it.
void expect_no_body(Probe& m, const fs::path& root, const std::string& path, int status,
                    std::string_view header, const std::vector<std::string>& fallbacks = {"/index.html"})
{
    expect(m, root, path, status, header, fallbacks, true);
    const auto wire = m.get(root, path, true, fallbacks).serialize();
    const auto end = wire.find("\r\n\r\n");
    check(end != std::string::npos && end + 4 == wire.size(), "HEAD \"" + path + "\": a body is sent");
}

void test_try_files()
{
    std::string tmpl = (fs::temp_directory_path() / "path_under_root_test.XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
        check(false, "mkdtemp " + tmpl);
        return;
    }
    const fs::path dir = tmpl;
    const fs::path root = dir / "www";
    const fs::path outside = dir / "outside.txt";

    std::error_code ec;
    const bool ready = write(root / "a.txt", "file-a")
                    && write(root / "index.html", "root-index")
                    && write(root / "d" / "index.html", "dir-index")
                    && fs::create_directories(root / "empty", ec)
                    && write(outside, std::string(kOutside))
                    && write(dir / "www2" / "x.txt", std::string(kOutside));
    check(ready, "the tree in " + dir.string() + " is written");
    if (!ready) {
        fs::remove_all(dir, ec);
        return;
    }

    Probe m;

    for (const fs::path& r : {root, fs::path(root.string() + "/")}) {
        // 3. As before.
        expect(m, r, "/a.txt", 200, "file-a");
        expect(m, r, "/", 200, "root-index");
        expect(m, r, "/d", 301, "Location: /d/");
        expect(m, r, "/d/", 200, "dir-index");
        expect(m, r, "/missing", 200, "root-index");
        expect(m, r, "/missing", 404, "404 Not Found", {});
        expect(m, r, "/empty/", 200, "root-index");
        expect(m, r, "/a.txt/", 200, "root-index");
        expect(m, r, "/missing/../a.txt", 200, "file-a");

        // 4. Not under the root: refused, nothing read.
        expect(m, r, "/" + outside.string(), 400, "400 Bad Request");
        expect(m, r, "//" + outside.string(), 400, "400 Bad Request");
        expect(m, r, "/../outside.txt", 400, "400 Bad Request");
        expect(m, r, "/d/../../outside.txt", 400, "400 Bad Request");
        expect(m, r, "/../www2/x.txt", 400, "400 Bad Request");
        expect(m, r, "/../www/a.txt", 400, "400 Bad Request");
        expect(m, r, "a.txt", 400, "400 Bad Request");
        expect(m, r, "", 400, "400 Bad Request");

        // A fallback not under the root is skipped.
        expect(m, r, "/missing", 404, "404 Not Found", {"/../outside.txt"});
        expect(m, r, "/missing", 404, "404 Not Found", {"/" + outside.string()});
        expect(m, r, "/missing", 200, "root-index", {"/../outside.txt", "/index.html"});
        expect(m, r, "/missing", 404, "404 Not Found", {""});
        expect(m, r, "/missing", 404, "404 Not Found", {"index.html"});

        // 5. HEAD.
        expect_no_body(m, r, "/a.txt", 200, "Content-Length: 6");
        expect_no_body(m, r, "/d", 301, "Location: /d/");
        expect_no_body(m, r, "//" + outside.string(), 400, "Content-Length: 15");
        expect_no_body(m, r, "/missing", 404, "Content-Length: 13", {});
    }

    // 3, 4. A relative root.
    const fs::path rel = fs::relative(root, fs::current_path(), ec);
    check(!ec && rel.is_relative(), "a relative path to " + root.string());
    if (!ec && rel.is_relative()) {
        expect(m, rel, "/a.txt", 200, "file-a");
        expect(m, rel, "/", 200, "root-index");
        expect(m, rel, "//" + outside.string(), 400, "400 Bad Request");
        expect(m, rel, "/../outside.txt", 400, "400 Bad Request");
    }

    fs::remove_all(dir, ec);
}

} // namespace

int main()
{
    test_table();
    test_try_files();

    if (g_failed) {
        std::printf("path_under_root_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("path_under_root_test: OK\n");
    return 0;
}
