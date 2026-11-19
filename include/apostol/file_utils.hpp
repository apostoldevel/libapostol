#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace apostol
{

/// Create directory tree (mkdir -p). Returns true if directory exists after call.
bool create_directories(const std::filesystem::path& path);

/// Remove a file (no-throw, silent if not exists).
void delete_file(const std::filesystem::path& path) noexcept;

/// Write binary data to file, creating parent directories as needed.
/// Returns true on success.
bool write_file(const std::filesystem::path& path, std::string_view data);

/// SHA256 hex digest (OpenSSL EVP).
std::string sha256_hex(std::string_view data);

/// Determine MIME type from file extension (with leading dot, e.g. ".html").
/// Returns "application/octet-stream" for unknown extensions.
std::string_view file_mime_type(std::string_view extension);

/// Check that a path component is safe (no "..", not empty, no null bytes).
bool is_safe_path(std::string_view path);

/// Resolve a request path (e.g. "/css/app.css") against a document root.
///
/// Returns root/<path less its first '/'>, the path part normalized lexically
/// ("/a//b", "/a/./b", "/a/../b" → root/a/b, root/a/b, root/b); "/" (and a
/// path normalizing to it) returns the root itself. The root is used as
/// given, not normalized; an empty root is ".".
///
/// Returns an empty path when @p path is empty, does not start with '/',
/// contains a NUL, starts with "//" (the rest would be an absolute path), or
/// leads out of the root through ".." ("/../x", "/a/../../x"). The check is
/// lexical: symbolic links inside the root are neither followed nor judged.
///
/// This is the check ApostolModule::try_files() applies; a caller's own
/// checks of the request path (e.g. is_safe_path) are in addition to it.
std::filesystem::path path_under_root(const std::filesystem::path& root, std::string_view path);

} // namespace apostol
