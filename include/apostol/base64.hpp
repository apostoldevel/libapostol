#pragma once

#include <string>
#include <string_view>

namespace apostol
{

/// Encode binary data to base64 (RFC 4648).
std::string base64_encode(std::string_view input);

/// Decode base64 string back to binary data (RFC 4648).
/// Ignores whitespace; padding is optional, but at most two '=' end the
/// data. Throws std::invalid_argument on a byte outside the alphabet, on a
/// third '=', or on data after the padding.
std::string base64_decode(std::string_view input);

} // namespace apostol
