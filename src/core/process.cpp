#include "apostol/process.hpp"

namespace apostol
{

std::string_view role_name(ProcessRole role) noexcept
{
    switch (role)
    {
        case ProcessRole::master:
            return "master";
        case ProcessRole::single:
            return "single";
        case ProcessRole::worker:
            return "worker";
        case ProcessRole::helper:
            return "helper";
        case ProcessRole::custom:
            return "custom";
        case ProcessRole::signaller:
            return "signaller";
    }
    return "unknown";
}

} // namespace apostol
