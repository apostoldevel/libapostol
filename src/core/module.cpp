#include "apostol/module.hpp"

namespace apostol
{

void ModuleManager::add_module(std::unique_ptr<Module> m)
{
    modules_.push_back(std::move(m));
}

bool ModuleManager::execute(const HttpRequest& req, HttpResponse& resp)
{
    if (stopped_)
        return false;

    for (auto& m : modules_) {
        if (!m->enabled())
            continue;
        if (m->execute(req, resp))
            return true;
    }
    return false;
}

void ModuleManager::heartbeat(std::chrono::system_clock::time_point now)
{
    if (stopped_)
        return;

    for (auto& m : modules_) {
        if (m->enabled())
            m->heartbeat(now);
    }
}

void ModuleManager::on_start()
{
    for (auto& m : modules_)
        if (m->enabled())
            m->on_start();
}

void ModuleManager::on_stop()
{
    for (auto& m : modules_)
        if (m->enabled())
            m->on_stop();

    stopped_ = true;
}

std::string ModuleManager::module_names() const
{
    std::string result;
    for (auto& m : modules_) {
        if (m->enabled()) {
            if (!result.empty()) result += ", ";
            result += '"';
            result += m->title();
            result += '"';
        }
    }
    return result;
}

} // namespace apostol
