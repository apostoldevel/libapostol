#include "apostol/route_manager.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace apostol
{

// --- RouteEntry (private implementation detail) ------------------------------

struct RouteManager::RouteEntry
{
    std::string method;
    std::string pattern;
    Handler     handler;
    RouteBuilder builder{*this};

    std::string summary;
    std::string description;
    std::vector<std::string> tags;
    bool is_deprecated{false};

    struct ParamInfo {
        std::string name;
        std::string in;
        std::string type;
        bool required{false};
        std::string description;
    };
    std::vector<ParamInfo> params;

    struct RequestBodyInfo {
        std::string content_type;
        nlohmann::json schema;
        bool required{true};
    };
    std::optional<RequestBodyInfo> request_body;

    struct ResponseInfo {
        int status;
        std::string description;
        nlohmann::json schema;
    };
    std::vector<ResponseInfo> responses;
};

// Out-of-line special members (RouteEntry must be complete)
RouteManager::RouteManager() = default;
RouteManager::~RouteManager() = default;
RouteManager::RouteManager(RouteManager&&) noexcept = default;
RouteManager& RouteManager::operator=(RouteManager&&) noexcept = default;

// --- RouteManager ------------------------------------------------------------

void RouteManager::set_base_path(std::string_view base)
{
    base_path_ = base;
    // Strip trailing slash (but keep "/" as-is)
    while (base_path_.size() > 1 && base_path_.back() == '/')
        base_path_.pop_back();
}

RouteBuilder& RouteManager::add_route(std::string_view method,
                                      std::string_view path,
                                      Handler handler)
{
    auto entry = std::make_unique<RouteEntry>();
    entry->method  = method;
    entry->pattern = path;
    entry->handler = std::move(handler);

    auto& ref = *entry;
    routes_.push_back(std::move(entry));
    return ref.builder;
}

bool RouteManager::dispatch(const HttpRequest& req, HttpResponse& resp)
{
    // Classify routes into three categories for priority dispatch
    enum class Kind { exact, parametric, wildcard };

    auto classify = [](std::string_view pattern) -> Kind {
        if (pattern.find('*') != std::string_view::npos)
            return Kind::wildcard;
        if (pattern.find('{') != std::string_view::npos)
            return Kind::parametric;
        return Kind::exact;
    };

    // 3-pass priority: exact > parametric > wildcard
    for (auto pass : {Kind::exact, Kind::parametric, Kind::wildcard}) {
        for (auto& entry : routes_) {
            // Build effective pattern: base_path + pattern
            std::string effective = base_path_ + entry->pattern;

            if (classify(effective) != pass)
                continue;

            // Method check
            if (entry->method != req.method)
                continue;

            // Path match
            PathParams params;
            if (match_route(effective, req.path, params)) {
                entry->handler(req, resp, params);
                return true;
            }
        }
    }

    return false;
}

bool RouteManager::has_route(std::string_view path) const
{
    for (auto& entry : routes_) {
        std::string effective = base_path_ + entry->pattern;
        PathParams params;
        if (match_route(effective, path, params))
            return true;
    }
    return false;
}

void RouteManager::set_info(std::string_view title, std::string_view version,
                            std::string_view description)
{
    api_title_       = title;
    api_version_     = version;
    api_description_ = description;
}

void RouteManager::add_server(std::string_view url, std::string_view description)
{
    servers_.push_back({std::string(url), std::string(description)});
}

nlohmann::json RouteManager::openapi_spec() const
{
    nlohmann::json spec;
    spec["openapi"] = "3.0.0";

    // Info
    spec["info"]["title"] = api_title_;
    spec["info"]["version"] = api_version_;
    if (!api_description_.empty())
        spec["info"]["description"] = api_description_;

    // Servers
    if (!servers_.empty()) {
        spec["servers"] = nlohmann::json::array();
        for (auto& s : servers_) {
            nlohmann::json srv;
            srv["url"] = s.url;
            if (!s.description.empty())
                srv["description"] = s.description;
            spec["servers"].push_back(std::move(srv));
        }
    }

    // Paths
    spec["paths"] = nlohmann::json::object();
    std::set<std::string> all_tags;

    for (auto& entry : routes_) {
        // Build effective path (base_path + pattern)
        std::string effective_path = base_path_ + entry->pattern;

        std::string method_lower;
        for (char c : entry->method)
            method_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto& op = spec["paths"][effective_path][method_lower];

        if (!entry->summary.empty())
            op["summary"] = entry->summary;
        if (!entry->description.empty())
            op["description"] = entry->description;
        if (entry->is_deprecated)
            op["deprecated"] = true;

        // Tags
        if (!entry->tags.empty()) {
            op["tags"] = entry->tags;
            for (auto& t : entry->tags)
                all_tags.insert(t);
        }

        // Parameters
        if (!entry->params.empty()) {
            op["parameters"] = nlohmann::json::array();
            for (auto& p : entry->params) {
                nlohmann::json param;
                param["name"] = p.name;
                param["in"] = p.in;
                param["required"] = p.required;
                param["schema"] = {{"type", p.type}};
                if (!p.description.empty())
                    param["description"] = p.description;
                op["parameters"].push_back(std::move(param));
            }
        }

        // Request body
        if (entry->request_body) {
            auto& rb = *entry->request_body;
            op["requestBody"]["required"] = rb.required;
            if (!rb.schema.empty())
                op["requestBody"]["content"][rb.content_type]["schema"] = rb.schema;
            else
                op["requestBody"]["content"][rb.content_type] = nlohmann::json::object();
        }

        // Responses
        if (!entry->responses.empty()) {
            op["responses"] = nlohmann::json::object();
            for (auto& r : entry->responses) {
                auto key = std::to_string(r.status);
                op["responses"][key]["description"] = r.description;
                if (!r.schema.empty())
                    op["responses"][key]["content"]["application/json"]["schema"] = r.schema;
            }
        }
    }

    // Top-level tags
    if (!all_tags.empty()) {
        spec["tags"] = nlohmann::json::array();
        for (auto& t : all_tags)
            spec["tags"].push_back({{"name", t}});
    }

    return spec;
}

bool RouteManager::match_route(std::string_view pattern,
                               std::string_view path,
                               PathParams& out)
{
    auto pat_segs = split_path(pattern);
    auto path_segs = split_path(path);

    // Check for wildcard at the end of pattern
    bool has_wildcard = !pat_segs.empty() && pat_segs.back() == "*";

    if (has_wildcard) {
        // Wildcard: path must have at least as many segments as pattern
        // (pattern segments minus the wildcard, plus at least one more)
        std::size_t prefix_len = pat_segs.size() - 1; // segments before '*'
        if (path_segs.size() <= prefix_len)
            return false;

        // Match all segments before the wildcard
        PathParams tmp;
        for (std::size_t i = 0; i < prefix_len; ++i) {
            auto seg = pat_segs[i];
            if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
                // Parametric segment
                auto name = seg.substr(1, seg.size() - 2);
                tmp.params[std::string(name)] = std::string(path_segs[i]);
            } else {
                // Exact segment match
                if (seg != path_segs[i])
                    return false;
            }
        }

        out = std::move(tmp);
        return true;
    }

    // Non-wildcard: segment counts must match exactly
    if (pat_segs.size() != path_segs.size())
        return false;

    PathParams tmp;
    for (std::size_t i = 0; i < pat_segs.size(); ++i) {
        auto seg = pat_segs[i];
        if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
            // Parametric segment — capture value
            auto name = seg.substr(1, seg.size() - 2);
            tmp.params[std::string(name)] = std::string(path_segs[i]);
        } else {
            // Exact segment match
            if (seg != path_segs[i])
                return false;
        }
    }

    out = std::move(tmp);
    return true;
}

std::vector<std::string_view> RouteManager::split_path(std::string_view path)
{
    std::vector<std::string_view> segments;

    // Skip leading slash
    std::size_t start = 0;
    if (!path.empty() && path[0] == '/')
        start = 1;

    while (start < path.size()) {
        auto pos = path.find('/', start);
        if (pos == std::string_view::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        if (pos > start)
            segments.push_back(path.substr(start, pos - start));
        start = pos + 1;
    }

    return segments;
}

// --- RouteBuilder ------------------------------------------------------------

RouteBuilder& RouteBuilder::summary(std::string_view s)
{
    entry_.summary = s;
    return *this;
}

RouteBuilder& RouteBuilder::description(std::string_view d)
{
    entry_.description = d;
    return *this;
}

RouteBuilder& RouteBuilder::tag(std::string_view t)
{
    entry_.tags.emplace_back(t);
    return *this;
}

RouteBuilder& RouteBuilder::deprecated(bool v)
{
    entry_.is_deprecated = v;
    return *this;
}

RouteBuilder& RouteBuilder::param(std::string_view name, std::string_view in,
                                  std::string_view type, bool required,
                                  std::string_view desc)
{
    entry_.params.push_back({
        std::string(name), std::string(in), std::string(type),
        required, std::string(desc)
    });
    return *this;
}

RouteBuilder& RouteBuilder::request_body(std::string_view content_type,
                                         const nlohmann::json& schema,
                                         bool required)
{
    entry_.request_body = RouteManager::RouteEntry::RequestBodyInfo{
        std::string(content_type), schema, required
    };
    return *this;
}

RouteBuilder& RouteBuilder::response(int status, std::string_view desc,
                                     const nlohmann::json& schema)
{
    entry_.responses.push_back({status, std::string(desc), schema});
    return *this;
}

} // namespace apostol
