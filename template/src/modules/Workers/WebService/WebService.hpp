#pragma once

#include "apostol/routed_module.hpp"

#include <string_view>

namespace apostol
{

class Application;
class PgPool;

class WebService final : public RoutedModule
{
public:
    explicit WebService(Application& app);

    std::string_view name()    const override { return "WebService"; }
    bool             enabled() const override { return true; }

protected:
    void init_routes() override;

private:
#ifdef WITH_POSTGRESQL
    PgPool* pool_{nullptr};
#endif
};

} // namespace apostol
