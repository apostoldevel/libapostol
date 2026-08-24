#include "apostol/oauth_providers.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace apostol
{

void OAuthProviders::load(const std::filesystem::path& oauth2_dir)
{
    namespace fs = std::filesystem;

    if (!fs::is_directory(oauth2_dir))
        return;

    for (const auto& entry : fs::directory_iterator(oauth2_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            if (!j.is_object())
                continue;

            auto provider = entry.path().stem().string();

            for (const auto& [section_name, section] : j.items()) {
                if (!section.is_object())
                    continue;

                OAuthApp app;
                app.provider = provider;
                app.name     = section_name;

                if (auto it = section.find("client_id"); it != section.end() && it->is_string())
                    app.client_id = it->get<std::string>();

                if (auto it = section.find("client_secret"); it != section.end() && it->is_string())
                    app.client_secret = it->get<std::string>();

                if (auto it = section.find("algorithm"); it != section.end() && it->is_string())
                    app.algorithm = it->get<std::string>();

                if (auto it = section.find("issuers"); it != section.end() && it->is_array())
                    for (const auto& iss : *it)
                        if (iss.is_string())
                            app.issuers.push_back(iss.get<std::string>());

                if (auto it = section.find("javascript_origins"); it != section.end() && it->is_array())
                    for (const auto& origin : *it)
                        if (origin.is_string())
                            app.javascript_origins.push_back(origin.get<std::string>());

                if (auto it = section.find("redirect_uris"); it != section.end() && it->is_array())
                    for (const auto& uri : *it)
                        if (uri.is_string())
                            app.redirect_uris.push_back(uri.get<std::string>());

                if (auto it = section.find("scopes"); it != section.end() && it->is_array())
                    for (const auto& sc : *it)
                        if (sc.is_string())
                            app.scopes.push_back(sc.get<std::string>());

                if (auto it = section.find("allowed_ips"); it != section.end() && it->is_array())
                    for (const auto& ip : *it)
                        if (ip.is_string())
                            app.allowed_ips.push_back(ip.get<std::string>());

                if (auto it = section.find("auth_uri"); it != section.end() && it->is_string())
                    app.auth_uri = it->get<std::string>();

                if (auto it = section.find("token_uri"); it != section.end() && it->is_string())
                    app.token_uri = it->get<std::string>();

                if (auto it = section.find("userinfo_uri"); it != section.end() && it->is_string())
                    app.userinfo_uri = it->get<std::string>();

                if (auto it = section.find("userinfo_audience"); it != section.end() && it->is_string())
                    app.userinfo_audience = it->get<std::string>();

                // Yandex calls the user id "id"; OpenID Connect calls it "sub".
                if (auto it = section.find("userinfo_subject"); it != section.end() && it->is_string())
                    app.userinfo_subject = it->get<std::string>();
                else
                    app.userinfo_subject = "sub";

                // Yandex answers to "Authorization: OAuth <token>", not Bearer.
                if (auto it = section.find("userinfo_scheme"); it != section.end() && it->is_string())
                    app.userinfo_scheme = it->get<std::string>();
                else
                    app.userinfo_scheme = "Bearer";

                // Sign-in-list fields. Read by names of their own; a file that sets
                // none of them describes no external provider and is simply absent
                // from the list (external defaults to false).
                if (auto it = section.find("login_scope"); it != section.end() && it->is_string())
                    app.login_scope = it->get<std::string>();

                if (auto it = section.find("display_name"); it != section.end() && it->is_string())
                    app.display_name = it->get<std::string>();

                if (auto it = section.find("icon"); it != section.end() && it->is_string())
                    app.icon = it->get<std::string>();

                if (auto it = section.find("external"); it != section.end() && it->is_boolean())
                    app.external = it->get<bool>();

                // cert_uri: check both "cert_uri" and Google's "auth_provider_x509_cert_url"
                if (auto it = section.find("cert_uri"); it != section.end() && it->is_string())
                    app.cert_uri = it->get<std::string>();
                else if (auto it2 = section.find("auth_provider_x509_cert_url"); it2 != section.end() && it2->is_string())
                    app.cert_uri = it2->get<std::string>();

                apps_.push_back(std::move(app));
            }
        } catch (...) {
            continue; // skip malformed JSON
        }
    }
}

void OAuthProviders::clear()
{
    apps_.clear();
}

const OAuthApp* OAuthProviders::find_by_client_id(std::string_view client_id) const
{
    for (const auto& app : apps_)
        if (app.client_id == client_id)
            return &app;
    return nullptr;
}

const OAuthApp* OAuthProviders::find_by_client_id(std::string_view client_id,
                                                 std::string_view provider) const
{
    for (const auto& app : apps_)
        if (app.provider == provider && app.client_id == client_id)
            return &app;
    return nullptr;
}

const OAuthApp* OAuthProviders::find_default_by_client_id(std::string_view client_id) const
{
    return find_by_client_id(client_id, "default");
}

const OAuthApp* OAuthProviders::find(std::string_view provider, std::string_view app_name) const
{
    for (const auto& app : apps_)
        if (app.provider == provider && app.name == app_name)
            return &app;
    return nullptr;
}

const OAuthApp* OAuthProviders::find_default(std::string_view app_name) const
{
    return find("default", app_name);
}

std::pair<std::string, std::string>
OAuthProviders::credentials(std::string_view app_name) const
{
    for (const auto& app : apps_)
        if (app.name == app_name && !app.client_id.empty() && !app.client_secret.empty())
            return {app.client_id, app.client_secret};
    return {};
}

std::vector<std::string> OAuthProviders::allowed_origins() const
{
    std::vector<std::string> result;
    for (const auto& app : apps_) {
        for (const auto& origin : app.javascript_origins) {
            bool found = false;
            for (const auto& existing : result)
                if (existing == origin) { found = true; break; }
            if (!found)
                result.push_back(origin);
        }
    }
    return result;
}

} // namespace apostol
