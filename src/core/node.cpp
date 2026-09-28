#include "apostol/node.hpp"

#include "apostol/http_utils.hpp"

#include <cstdlib>
#include <unistd.h>

#include <fmt/format.h>

namespace apostol
{

std::string node_id(std::string_view configured)
{
    if (!configured.empty())
        return std::string(configured);

    if (const char* env = std::getenv("NODE_NAME"); env && *env)
        return env;

    return get_hostname();
}

std::string node_process_id(std::string_view configured)
{
    return fmt::format("{}:{}", node_id(configured), ::getpid());
}

} // namespace apostol
