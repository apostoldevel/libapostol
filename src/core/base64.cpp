#include "apostol/base64.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace apostol
{

// ─── Base64 encode/decode (RFC 4648) ─────────────────────────────────────────

static constexpr char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view input)
{
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    auto* p = reinterpret_cast<const uint8_t*>(input.data());
    auto  len = input.size();

    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t n = (static_cast<uint32_t>(p[i]) << 16)
                   | (static_cast<uint32_t>(p[i + 1]) << 8)
                   |  static_cast<uint32_t>(p[i + 2]);
        out += kEncodeTable[(n >> 18) & 0x3F];
        out += kEncodeTable[(n >> 12) & 0x3F];
        out += kEncodeTable[(n >>  6) & 0x3F];
        out += kEncodeTable[ n        & 0x3F];
    }

    if (i < len) {
        uint32_t n = static_cast<uint32_t>(p[i]) << 16;
        if (i + 1 < len)
            n |= static_cast<uint32_t>(p[i + 1]) << 8;

        out += kEncodeTable[(n >> 18) & 0x3F];
        out += kEncodeTable[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? kEncodeTable[(n >> 6) & 0x3F] : '=';
        out += '=';
    }

    return out;
}

static constexpr std::array<int, 256> build_decode_table()
{
    std::array<int, 256> table{};
    for (auto& v : table)
        v = -1;

    for (int i = 0; i < 26; ++i)
        table[static_cast<unsigned char>('A' + i)] = i;
    for (int i = 0; i < 26; ++i)
        table[static_cast<unsigned char>('a' + i)] = i + 26;
    for (int i = 0; i < 10; ++i)
        table[static_cast<unsigned char>('0' + i)] = i + 52;

    table[static_cast<unsigned char>('+')] = 62;
    table[static_cast<unsigned char>('/')] = 63;

    return table;
}

static constexpr auto kDecodeTable = build_decode_table();

std::string base64_decode(std::string_view input)
{
    std::string out;
    out.reserve((input.size() / 4) * 3);

    uint32_t accum = 0;
    int bits = 0;

    for (char c : input) {
        // Skip whitespace
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;

        // Padding — stop decoding
        if (c == '=')
            break;

        int val = kDecodeTable[static_cast<unsigned char>(c)];
        if (val < 0)
            throw std::invalid_argument("invalid base64 character");

        accum = (accum << 6) | static_cast<uint32_t>(val);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accum >> bits) & 0xFF);
        }
    }

    return out;
}

} // namespace apostol
