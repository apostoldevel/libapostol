#include "apostol/yaml_writer.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace apostol
{

namespace
{

// YAML reserved words that must be quoted when used as string values
bool needs_quotes(std::string_view s)
{
    if (s.empty())
        return true;

    // Looks like a number? (includes scientific notation: 1e5, 2.5E-3)
    bool looks_numeric = std::all_of(s.begin(), s.end(),
        [](unsigned char c) { return std::isdigit(c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'; });
    if (looks_numeric && !s.empty())
        return true;

    // YAML boolean words
    static const char* reserved[] = {
        "true", "false", "yes", "no", "on", "off", "null", "~",
        "True", "False", "Yes", "No", "On", "Off", "Null",
        "TRUE", "FALSE", "YES", "NO", "ON", "OFF", "NULL"
    };
    for (auto r : reserved)
        if (s == r) return true;

    // Contains special chars?
    for (char c : s)
        if (c == ':' || c == '#' || c == '[' || c == ']'
            || c == '{' || c == '}' || c == ',' || c == '&'
            || c == '*' || c == '!' || c == '|' || c == '>'
            || c == '\'' || c == '"' || c == '%' || c == '@'
            || c == '\n' || c == '\r')
            return true;

    // Starts or ends with whitespace?
    if (std::isspace(static_cast<unsigned char>(s.front()))
        || std::isspace(static_cast<unsigned char>(s.back())))
        return true;

    return false;
}

std::string quote(std::string_view s)
{
    std::string result = "'";
    for (char c : s) {
        if (c == '\'')
            result += "''";
        else
            result += c;
    }
    result += '\'';
    return result;
}

// Helper: is a compound value that should be rendered on subsequent lines?
bool is_block_value(const nlohmann::json& v)
{
    return (v.is_object() && !v.empty()) || (v.is_array() && !v.empty());
}

void write_yaml(std::ostringstream& os, const nlohmann::json& j,
                int indent, bool is_array_item)
{
    std::string pad(static_cast<std::size_t>(indent), ' ');
    std::string item_pad = is_array_item
        ? std::string(static_cast<std::size_t>(std::max(0, indent - 2)), ' ') + "- "
        : pad;

    if (j.is_object()) {
        if (j.empty()) {
            os << "{}";
            return;
        }
        bool first = true;
        for (auto& [key, val] : j.items()) {
            // Determine line prefix
            if (first && is_array_item) {
                os << item_pad;
            } else {
                os << pad;
            }

            os << key << ":";

            if (is_block_value(val)) {
                os << "\n";
                write_yaml(os, val, indent + 2, false);
            } else {
                os << " ";
                write_yaml(os, val, indent + 2, false);
                os << "\n";
            }

            first = false;
        }
    } else if (j.is_array()) {
        if (j.empty()) {
            os << "[]";
            return;
        }
        for (auto& item : j) {
            if (item.is_object() && !item.empty()) {
                write_yaml(os, item, indent + 2, true);
            } else {
                os << pad << "- ";
                write_yaml(os, item, indent + 2, false);
                os << "\n";
            }
        }
    } else if (j.is_string()) {
        auto s = j.get<std::string>();
        if (needs_quotes(s))
            os << quote(s);
        else
            os << s;
    } else if (j.is_boolean()) {
        os << (j.get<bool>() ? "true" : "false");
    } else if (j.is_number_integer()) {
        os << j.get<int64_t>();
    } else if (j.is_number_unsigned()) {
        os << j.get<uint64_t>();
    } else if (j.is_number_float()) {
        os << j.get<double>();
    } else {
        os << "null";
    }
}

} // anonymous namespace

std::string json_to_yaml(const nlohmann::json& j, int indent)
{
    std::ostringstream os;
    write_yaml(os, j, indent, false);
    auto result = os.str();
    // Ensure trailing newline
    if (!result.empty() && result.back() != '\n')
        result += '\n';
    return result;
}

} // namespace apostol
