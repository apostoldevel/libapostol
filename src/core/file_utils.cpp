#include "apostol/file_utils.hpp"

#include <fstream>
#include <unordered_map>

#ifdef WITH_SSL
#include <openssl/evp.h>
#endif

namespace apostol
{

// ─── create_directories ──────────────────────────────────────────────────────

bool create_directories(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return std::filesystem::is_directory(path);
}

// ─── delete_file ─────────────────────────────────────────────────────────────

void delete_file(const std::filesystem::path& path) noexcept
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ─── write_file ──────────────────────────────────────────────────────────────

bool write_file(const std::filesystem::path& path, std::string_view data)
{
    // Ensure parent directories exist
    if (path.has_parent_path()) {
        if (!apostol::create_directories(path.parent_path()))
            return false;
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return false;

    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

// ─── sha256_hex ──────────────────────────────────────────────────────────────

std::string sha256_hex(std::string_view data)
{
#ifdef WITH_SSL
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    auto* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    EVP_MD_CTX_free(ctx);

    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash_len * 2);
    for (unsigned int i = 0; i < hash_len; ++i) {
        result += hex_chars[(hash[i] >> 4) & 0x0F];
        result += hex_chars[hash[i] & 0x0F];
    }
    return result;
#else
    (void)data;
    return {};
#endif
}

// ─── file_mime_type ──────────────────────────────────────────────────────────

std::string_view file_mime_type(std::string_view extension)
{
    // Same table as ApostolModule::mime_type() but as a free function
    static const std::unordered_map<std::string_view, std::string_view> types{
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".css",   "text/css"},
        {".js",    "application/javascript"},
        {".mjs",   "application/javascript"},
        {".json",  "application/json"},
        {".xml",   "application/xml"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".webp",  "image/webp"},
        {".svg",   "image/svg+xml"},
        {".ico",   "image/x-icon"},
        {".txt",   "text/plain; charset=utf-8"},
        {".md",    "text/plain; charset=utf-8"},
        {".pdf",   "application/pdf"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
        {".eot",   "application/vnd.ms-fontobject"},
        {".mp4",   "video/mp4"},
        {".webm",  "video/webm"},
        {".gz",    "application/gzip"},
        {".zip",   "application/zip"},
        {".doc",   "application/msword"},
        {".docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls",   "application/vnd.ms-excel"},
        {".xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".csv",   "text/csv"},
    };

    auto it = types.find(extension);
    return it != types.end() ? it->second : "application/octet-stream";
}

// ─── is_safe_path ────────────────────────────────────────────────────────────

bool is_safe_path(std::string_view path)
{
    if (path.empty())
        return false;

    // Check for null bytes
    if (path.find('\0') != std::string_view::npos)
        return false;

    // Check for ".." path traversal
    if (path.find("..") != std::string_view::npos)
        return false;

    return true;
}

} // namespace apostol
