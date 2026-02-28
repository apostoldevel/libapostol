#include "apostol/routed_module.hpp"
#include "apostol/yaml_writer.hpp"

namespace apostol
{

namespace
{

std::string swagger_ui_html(std::string_view spec_url)
{
    std::string html = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>API Documentation</title>
<link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
<div id="swagger-ui"></div>
<script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
<script>
SwaggerUIBundle({
  url: ')";
    html += spec_url;
    html += R"(',
  dom_id: '#swagger-ui',
  presets: [SwaggerUIBundle.presets.apis, SwaggerUIBundle.SwaggerUIStandalonePreset],
  layout: 'BaseLayout'
});
</script>
</body>
</html>)";
    return html;
}

/// Normalize path: strip trailing slashes (keep at least "/")
std::string_view normalize(std::string_view p)
{
    while (p.size() > 1 && p.back() == '/')
        p.remove_suffix(1);
    return p;
}

} // anonymous namespace

void RoutedModule::set_docs_path(std::string_view path)
{
    docs_path_ = path;
    while (!docs_path_.empty() && docs_path_.back() == '/')
        docs_path_.pop_back();
}

void RoutedModule::init_methods()
{
    // Let the subclass register routes
    init_routes();

    // Register handlers for all standard methods.
    // dispatch() will return false for unregistered method+path combos.
    for (auto method : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"}) {
        add_method(method,
            [this](const HttpRequest& req, HttpResponse& resp) {
                // Try docs endpoints first (they live outside base_path)
                if (req.method == "GET" && try_docs_dispatch(req, resp))
                    return;

                if (!routes_.dispatch(req, resp)) {
                    // Path matched (check_location passed) but no route for this method → 405
                    method_not_allowed(req, resp);
                }
            });
    }
}

bool RoutedModule::check_location(const HttpRequest& req) const
{
    if (is_docs_path(req.path))
        return true;
    return routes_.has_route(req.path);
}

bool RoutedModule::is_docs_path(std::string_view path) const
{
    auto norm = normalize(path);
    auto json_path = docs_path_ + "/api.json";
    auto yaml_path = docs_path_ + "/api.yaml";

    return norm == docs_path_ || norm == json_path || norm == yaml_path;
}

bool RoutedModule::try_docs_dispatch(const HttpRequest& req,
                                     HttpResponse& resp) const
{
    auto norm = normalize(req.path);

    if (norm == docs_path_) {
        auto spec_url = docs_path_ + "/api.json";
        resp.set_status(HttpStatus::ok)
            .set_body(swagger_ui_html(spec_url), "text/html; charset=utf-8");
        return true;
    }

    auto json_path = docs_path_ + "/api.json";
    if (norm == json_path) {
        resp.set_status(HttpStatus::ok)
            .set_body(routes_.openapi_spec().dump(2), "application/json");
        return true;
    }

    auto yaml_path = docs_path_ + "/api.yaml";
    if (norm == yaml_path) {
        resp.set_status(HttpStatus::ok)
            .set_body(json_to_yaml(routes_.openapi_spec()),
                      "application/x-yaml; charset=utf-8");
        return true;
    }

    return false;
}

} // namespace apostol
