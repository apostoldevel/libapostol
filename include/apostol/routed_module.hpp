#pragma once

#include "apostol/apostol_module.hpp"
#include "apostol/route_manager.hpp"

namespace apostol
{

// RoutedModule — base class for modules with path-based routing + OpenAPI metadata.
// Inherits ApostolModule (CORS, method dispatch, etc.) and adds RouteManager.
// Modules override init_routes() instead of init_methods().
// check_location() automatically delegates to routes_.has_route().
class RoutedModule : public ApostolModule
{
public:
    using ApostolModule::ApostolModule;

    /// Access to RouteManager (e.g. for openapi_spec())
    const RouteManager& routes() const { return routes_; }

    /// Set custom docs endpoint path (default: "/docs")
    void set_docs_path(std::string_view path);

protected:
    std::string docs_path_{"/docs"};  // configurable via set_docs_path()
    RouteManager routes_;

    /// Override this to register routes (called once, lazily).
    virtual void init_routes() = 0;

    /// Wired automatically: delegates to routes_.dispatch().
    void init_methods() final;

    /// Delegates to routes_.has_route(req.path), also matches docs paths.
    bool check_location(const HttpRequest& req) const override;

private:
    bool is_docs_path(std::string_view path) const;
    bool try_docs_dispatch(const HttpRequest& req, HttpResponse& resp) const;
};

} // namespace apostol
