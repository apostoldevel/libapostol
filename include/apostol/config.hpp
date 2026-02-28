#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace apostol
{

// ─── ConfigError ─────────────────────────────────────────────────────────────

struct ConfigError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// ─── Config ──────────────────────────────────────────────────────────────────

// Type-safe wrapper around a parsed JSON config.
//
// JSON values may contain environment variable references: "${VAR_NAME}"
// which are expanded at read time.
//
// Example config.json:
// {
//   "daemon": { "pid": "/run/apostol.pid", "user": "www" },
//   "log":    { "path": "/var/log/apostol/error.log", "level": "info" },
//   "server": { "address": "0.0.0.0", "port": 8080 },
//   "database": {
//     "host":     "localhost",
//     "port":     5432,
//     "name":     "${DB_NAME}",
//     "user":     "${DB_USER}",
//     "password": "${DB_PASSWORD}"
//   }
// }
class Config
{
public:
    Config() = default;

    // Load from file; throws ConfigError on failure
    static Config from_file(const std::filesystem::path& path);

    // Load from JSON string; throws ConfigError on failure
    static Config from_string(std::string_view json);

    // Reload from the same file path (used for SIGHUP hot-reload)
    void reload();

    // Return the source path (empty if loaded from string)
    const std::filesystem::path& path() const noexcept { return path_; }

    // ── Type-safe accessors ───────────────────────────────────────────────────

    // Get required string value.  Throws ConfigError if missing or wrong type.
    std::string get_string(std::string_view key_path) const;

    // Get optional string value.
    std::optional<std::string> get_string_opt(std::string_view key_path) const;

    // Get string with default fallback.
    std::string get_string(std::string_view key_path, std::string_view default_value) const;

    // Get required integer value.
    std::int64_t get_int(std::string_view key_path) const;

    // Get integer with default fallback.
    std::int64_t get_int(std::string_view key_path, std::int64_t default_value) const;

    // Get required bool value.
    bool get_bool(std::string_view key_path) const;

    // Get bool with default fallback.
    bool get_bool(std::string_view key_path, bool default_value) const;

    // Get a sub-object as a new Config view.
    Config get_section(std::string_view key_path) const;

    // Check whether a key path exists.
    bool has(std::string_view key_path) const noexcept;

    // Direct access to underlying JSON (read-only).
    const nlohmann::json& json() const noexcept { return data_; }

private:
    explicit Config(nlohmann::json data, std::filesystem::path path = {});

    // Traverse dot-separated path ("server.port") and return the node.
    // Returns nullptr if any segment is missing.
    const nlohmann::json* navigate(std::string_view key_path) const noexcept;

    // Expand "${VAR}" references in a string value.
    static std::string expand_env(std::string_view value);

    // Recursively expand all string values in a JSON tree.
    static nlohmann::json expand_all(const nlohmann::json& node);

    nlohmann::json data_;
    std::filesystem::path path_;
};

} // namespace apostol
