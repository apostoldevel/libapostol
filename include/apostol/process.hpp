#pragma once

#include <cstdint>
#include <string>
#include <unistd.h>

namespace apostol
{

enum class ProcessRole : std::uint8_t
{
    master,
    single,    // no fork — all in one process (default)
    worker,
    helper,
    custom,    // user-defined background process
    signaller, // -s signal flag process (never enters a run loop)
};

std::string_view role_name(ProcessRole role) noexcept;

// ─── ChildInfo ────────────────────────────────────────────────────────────────
// Metadata about a child process managed by the master.
struct ChildInfo
{
    pid_t pid{-1};
    ProcessRole role{ProcessRole::worker};
    std::string name;
    bool shutting_down{false}; // true → do not respawn when it exits
};

} // namespace apostol
