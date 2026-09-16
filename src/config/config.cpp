#include "apostol/config.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace apostol
{

// ─── Construction ─────────────────────────────────────────────────────────────

Config::Config(nlohmann::json data, std::filesystem::path path) :
    data_(std::move(data)), path_(std::move(path))
{
}

Config Config::from_file(const std::filesystem::path& path)
{
    std::ifstream f(path);
    if (!f)
        throw ConfigError(fmt::format("cannot open config file: '{}'", path.string()));

    nlohmann::json raw;
    try
    {
        raw = nlohmann::json::parse(f, /*cb=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        throw ConfigError(fmt::format("JSON parse error in '{}': {}", path.string(), e.what()));
    }

    return Config(expand_all(raw), path);
}

Config Config::from_string(std::string_view json)
{
    nlohmann::json raw;
    try
    {
        raw = nlohmann::json::parse(json, nullptr, true, true);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        throw ConfigError(fmt::format("JSON parse error: {}", e.what()));
    }

    return Config(expand_all(raw));
}

void Config::reload()
{
    if (path_.empty())
        throw ConfigError("cannot reload: config was not loaded from a file");
    *this = from_file(path_);
}

// ─── Navigation ──────────────────────────────────────────────────────────────

const nlohmann::json* Config::navigate(std::string_view key_path) const noexcept
{
    const nlohmann::json* node = &data_;

    std::string_view remaining = key_path;
    while (!remaining.empty())
    {
        auto dot = remaining.find('.');
        std::string_view segment = remaining.substr(0, dot);
        remaining = (dot == std::string_view::npos) ? "" : remaining.substr(dot + 1);

        if (!node->is_object())
            return nullptr;

        auto it = node->find(segment);
        if (it == node->end())
            return nullptr;

        node = &(*it);
    }

    return node;
}

bool Config::has(std::string_view key_path) const noexcept
{
    return navigate(key_path) != nullptr;
}

// ─── String accessors ────────────────────────────────────────────────────────

std::string Config::get_string(std::string_view key_path) const
{
    const auto* node = navigate(key_path);
    if (!node)
        throw ConfigError(fmt::format("required config key '{}' not found", key_path));
    if (!node->is_string())
        throw ConfigError(fmt::format("config key '{}' must be a string, got {}", key_path, node->type_name()));
    return node->get<std::string>();
}

std::optional<std::string> Config::get_string_opt(std::string_view key_path) const
{
    const auto* node = navigate(key_path);
    if (!node || node->is_null())
        return std::nullopt;
    if (!node->is_string())
        throw ConfigError(fmt::format("config key '{}' must be a string, got {}", key_path, node->type_name()));
    return node->get<std::string>();
}

std::string Config::get_string(std::string_view key_path, std::string_view default_value) const
{
    const auto* node = navigate(key_path);
    if (!node || node->is_null())
        return std::string(default_value);
    if (!node->is_string())
        throw ConfigError(fmt::format("config key '{}' must be a string, got {}", key_path, node->type_name()));
    return node->get<std::string>();
}

// ─── Values taken from the environment ───────────────────────────────────────
//
// expand_all substitutes "${VAR}" inside string nodes only, so a flag or a
// number written as "${VAR}" reaches the accessor as a string: get_bool and
// get_int read it. An unset variable expands to "" — an empty or blank string
// is an absent key to the defaulted forms and an error to the required ones.

namespace
{

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// true|false|1|0|yes|no|on|off, any case, surrounding blanks ignored.
std::optional<bool> parse_bool(std::string_view s)
{
    std::string w;
    for (char c : trim(s))
        w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "true" || w == "1" || w == "yes" || w == "on")
        return true;
    if (w == "false" || w == "0" || w == "no" || w == "off")
        return false;
    return std::nullopt;
}

// A whole decimal integer with an optional sign; nothing else. Sets
// @p out_of_range when it is an integer but does not fit in 64 bits.
std::optional<std::int64_t> parse_int(std::string_view s, bool& out_of_range)
{
    out_of_range = false;
    s = trim(s);
    if (s.size() > 1 && s.front() == '+' && std::isdigit(static_cast<unsigned char>(s[1])))
        s.remove_prefix(1);
    if (s.empty())
        return std::nullopt;
    std::int64_t v{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (end != s.data() + s.size())
        return std::nullopt;
    if (ec == std::errc::result_out_of_range)
    {
        out_of_range = true;
        return std::nullopt;
    }
    if (ec != std::errc{})
        return std::nullopt;
    return v;
}

// The offending value as an error message shows it: trimmed, one line,
// no longer than a glance — it came from the environment, and a stray
// newline or a paste of the wrong variable must not break the log.
std::string shown(std::string_view s)
{
    std::string out;
    for (char c : trim(s).substr(0, 64))
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    if (trim(s).size() > 64)
        out += "...";
    return out;
}

[[noreturn]] void throw_not_int(std::string_view key_path, const std::string& s, bool out_of_range)
{
    if (out_of_range)
        throw ConfigError(fmt::format("config key '{}' is out of range for a 64-bit integer: '{}'",
                                      key_path, shown(s)));
    throw ConfigError(
        fmt::format("config key '{}' must be an integer, got string '{}'", key_path, shown(s)));
}

} // namespace

// ─── Integer accessors ───────────────────────────────────────────────────────

std::int64_t Config::get_int(std::string_view key_path) const
{
    const auto* node = navigate(key_path);
    if (!node)
        throw ConfigError(fmt::format("required config key '{}' not found", key_path));
    if (node->is_string())
    {
        const auto& s = node->get_ref<const std::string&>();
        if (trim(s).empty())
            throw ConfigError(fmt::format(
                "required config key '{}' is empty (an unset environment variable?)", key_path));
        bool out_of_range = false;
        if (auto v = parse_int(s, out_of_range))
            return *v;
        throw_not_int(key_path, s, out_of_range);
    }
    if (!node->is_number_integer())
        throw ConfigError(
            fmt::format("config key '{}' must be an integer, got {}", key_path, node->type_name()));
    return node->get<std::int64_t>();
}

std::int64_t Config::get_int(std::string_view key_path, std::int64_t default_value) const
{
    const auto* node = navigate(key_path);
    if (!node || node->is_null())
        return default_value;
    if (node->is_string())
    {
        const auto& s = node->get_ref<const std::string&>();
        if (trim(s).empty())
            return default_value;
        bool out_of_range = false;
        if (auto v = parse_int(s, out_of_range))
            return *v;
        throw_not_int(key_path, s, out_of_range);
    }
    if (!node->is_number_integer())
        throw ConfigError(
            fmt::format("config key '{}' must be an integer, got {}", key_path, node->type_name()));
    return node->get<std::int64_t>();
}

// ─── Bool accessors ──────────────────────────────────────────────────────────

bool Config::get_bool(std::string_view key_path) const
{
    const auto* node = navigate(key_path);
    if (!node)
        throw ConfigError(fmt::format("required config key '{}' not found", key_path));
    if (node->is_string())
    {
        const auto& s = node->get_ref<const std::string&>();
        if (trim(s).empty())
            throw ConfigError(fmt::format(
                "required config key '{}' is empty (an unset environment variable?)", key_path));
        if (auto v = parse_bool(s))
            return *v;
        throw ConfigError(fmt::format(
            "config key '{}' must be a bool (true|false|1|0|yes|no|on|off), got string '{}'",
            key_path, shown(s)));
    }
    if (!node->is_boolean())
        throw ConfigError(fmt::format("config key '{}' must be a bool, got {}", key_path, node->type_name()));
    return node->get<bool>();
}

bool Config::get_bool(std::string_view key_path, bool default_value) const
{
    const auto* node = navigate(key_path);
    if (!node || node->is_null())
        return default_value;
    if (node->is_string())
    {
        const auto& s = node->get_ref<const std::string&>();
        if (trim(s).empty())
            return default_value;
        if (auto v = parse_bool(s))
            return *v;
        throw ConfigError(fmt::format(
            "config key '{}' must be a bool (true|false|1|0|yes|no|on|off), got string '{}'",
            key_path, shown(s)));
    }
    if (!node->is_boolean())
        throw ConfigError(fmt::format("config key '{}' must be a bool, got {}", key_path, node->type_name()));
    return node->get<bool>();
}

// ─── Section accessor ────────────────────────────────────────────────────────

Config Config::get_section(std::string_view key_path) const
{
    const auto* node = navigate(key_path);
    if (!node)
        throw ConfigError(fmt::format("required config section '{}' not found", key_path));
    if (!node->is_object())
        throw ConfigError(
            fmt::format("config key '{}' must be an object, got {}", key_path, node->type_name()));
    return Config(*node, path_);
}

// ─── Env expansion ───────────────────────────────────────────────────────────

std::string Config::expand_env(std::string_view value)
{
    // Match ${VAR_NAME} or $VAR_NAME patterns
    static const std::regex env_re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*))");

    std::string result(value);
    std::smatch match;
    std::string::const_iterator search_start(result.cbegin());

    std::string out;
    out.reserve(result.size());

    auto it = result.cbegin();
    while (std::regex_search(it, result.cend(), match, env_re))
    {
        out.append(it, match[0].first);

        std::string var_name = match[1].matched ? match[1].str() : match[2].str();
        const char* env_val = std::getenv(var_name.c_str());
        if (env_val)
            out.append(env_val);
        // If not found, substitute empty string (silent — env vars may be optional)

        it = match[0].second;
    }
    out.append(it, result.cend());
    return out;
}

nlohmann::json Config::expand_all(const nlohmann::json& node)
{
    if (node.is_string())
        return expand_env(node.get<std::string>());

    if (node.is_object())
    {
        nlohmann::json obj = nlohmann::json::object();
        for (auto& [key, val] : node.items())
            obj[key] = expand_all(val);
        return obj;
    }

    if (node.is_array())
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto& val : node)
            arr.push_back(expand_all(val));
        return arr;
    }

    return node;
}

} // namespace apostol
