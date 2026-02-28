#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace apostol
{

/// Convert a nlohmann::json value to YAML string.
/// Handles the subset needed for OpenAPI specs: objects, arrays,
/// strings, numbers, booleans, null.
std::string json_to_yaml(const nlohmann::json& j, int indent = 0);

} // namespace apostol
