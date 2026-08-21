#include "apostol/site_config.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace apostol
{

void SiteConfigs::load(const std::filesystem::path& sites_dir)
{
    namespace fs = std::filesystem;

    if (!fs::is_directory(sites_dir))
        return;

    for (const auto& entry : fs::directory_iterator(sites_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            if (!j.is_object())
                continue;

            SiteConfig site;

            if (auto it = j.find("hosts"); it != j.end() && it->is_array())
                for (const auto& h : *it)
                    if (h.is_string())
                        site.hosts.push_back(h.get<std::string>());

            if (auto it = j.find("root"); it != j.end() && it->is_string())
                site.root = it->get<std::string>();

            if (auto it = j.find("oauth2"); it != j.end() && it->is_object()) {
                const auto& o = *it;
                if (auto v = o.find("identifier"); v != o.end() && v->is_string())
                    site.oauth2.identifier = v->get<std::string>();
                if (auto v = o.find("secret"); v != o.end() && v->is_string())
                    site.oauth2.secret = v->get<std::string>();
                if (auto v = o.find("consent"); v != o.end() && v->is_string())
                    site.oauth2.consent = v->get<std::string>();
                if (auto v = o.find("callback"); v != o.end() && v->is_string())
                    site.oauth2.callback = v->get<std::string>();
                if (auto v = o.find("error"); v != o.end() && v->is_string())
                    site.oauth2.error = v->get<std::string>();
                if (auto v = o.find("debug"); v != o.end() && v->is_string())
                    site.oauth2.debug = v->get<std::string>();
            }

            sites_.push_back(std::move(site));
        } catch (...) {
            continue; // skip malformed JSON
        }
    }
}

void SiteConfigs::clear()
{
    sites_.clear();
}

const SiteConfig* SiteConfigs::find(std::string_view hostname) const
{
    for (const auto& site : sites_)
        for (const auto& host : site.hosts)
            if (host == hostname)
                return &site;
    return nullptr;
}

} // namespace apostol
