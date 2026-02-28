#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── SiteConfig ──────────────────────────────────────────────────────────────
//
// Per-site configuration loaded from conf/sites/*.json.
// Mirrors v1 GetSiteConfig(hostname).
//

struct SiteOAuth2
{
    std::string identifier;  // login page URL
    std::string secret;      // password page URL
    std::string callback;    // success redirect URL
    std::string error;       // error redirect URL
    std::string debug;       // debug redirect URL
};

struct SiteConfig
{
    std::vector<std::string> hosts;
    std::string root;
    SiteOAuth2 oauth2;
};

class SiteConfigs
{
public:
    /// Load all *.json files from @p sites_dir.
    /// Non-existent directory or malformed files are silently skipped.
    void load(const std::filesystem::path& sites_dir);

    /// Clear all loaded configurations (call before reload).
    void clear();

    /// Find site config by hostname (checks against hosts list).
    /// Returns nullptr if not found.
    const SiteConfig* find(std::string_view hostname) const;

    /// All loaded site configs.
    const std::vector<SiteConfig>& configs() const noexcept { return sites_; }

private:
    std::vector<SiteConfig> sites_;
};

} // namespace apostol
