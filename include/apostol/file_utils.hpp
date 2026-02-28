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

} // namespace apostol
