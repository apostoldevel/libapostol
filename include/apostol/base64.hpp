#pragma once

#include <string>
#include <string_view>

namespace apostol
{

/// Encode binary data to base64 (RFC 4648).
std::string base64_encode(std::string_view input);

/// Decode base64 string back to binary data (RFC 4648).
/// Ignores whitespace. Throws std::invalid_argument on invalid input.
std::string base64_decode(std::string_view input);

} // namespace apostol
