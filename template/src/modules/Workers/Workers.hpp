#pragma once

#include "apostol/application.hpp"
#include "WebService/WebService.hpp"

namespace apostol
{

static inline void create_workers(Application& app)
{
    app.module_manager().add_module(std::make_unique<WebService>(app));
}

} // namespace apostol
