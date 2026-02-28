#pragma once

#include "apostol/http.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace apostol
{

struct PathParams
{
    std::unordered_map<std::string, std::string> params;

    std::string operator[](std::string_view name) const
    {
        auto it = params.find(std::string(name));
        return it != params.end() ? it->second : std::string{};
    }

    bool has(std::string_view name) const
    {
        return params.contains(std::string(name));
    }
};

class RouteBuilder;

class RouteManager
{
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&,
                                       const PathParams&)>;

    RouteManager();
    ~RouteManager();

    RouteManager(RouteManager&&) noexcept;
    RouteManager& operator=(RouteManager&&) noexcept;

    void set_base_path(std::string_view base);
    const std::string& base_path() const { return base_path_; }

    RouteBuilder& add_route(std::string_view method, std::string_view path,
                            Handler handler);

    bool dispatch(const HttpRequest& req, HttpResponse& resp);
    bool has_route(std::string_view path) const;

    void set_info(std::string_view title, std::string_view version,
                  std::string_view description = "");
    void add_server(std::string_view url, std::string_view description = "");

    nlohmann::json openapi_spec() const;

    std::size_t size() const noexcept { return routes_.size(); }

private:
    friend class RouteBuilder;

    struct RouteEntry;

    static bool match_route(std::string_view pattern, std::string_view path,
                            PathParams& out);
    static std::vector<std::string_view> split_path(std::string_view path);

    std::string base_path_;
    std::vector<std::unique_ptr<RouteEntry>> routes_;

    std::string api_title_;
    std::string api_version_{"1.0.0"};
    std::string api_description_;

    struct ServerEntry { std::string url; std::string description; };
    std::vector<ServerEntry> servers_;
};

class RouteBuilder
{
public:
    RouteBuilder& summary(std::string_view s);
    RouteBuilder& description(std::string_view d);
    RouteBuilder& tag(std::string_view t);
    RouteBuilder& deprecated(bool v = true);

    RouteBuilder& param(std::string_view name, std::string_view in,
                        std::string_view type, bool required = false,
                        std::string_view desc = "");

    RouteBuilder& request_body(std::string_view content_type,
                               const nlohmann::json& schema = {},
                               bool required = true);

    RouteBuilder& response(int status, std::string_view desc,
                           const nlohmann::json& schema = {});

private:
    friend class RouteManager;
    explicit RouteBuilder(RouteManager::RouteEntry& entry) : entry_(entry) {}
    RouteManager::RouteEntry& entry_;
};

} // namespace apostol
