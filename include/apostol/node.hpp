#pragma once

#include <string>
#include <string_view>

namespace apostol
{

// ─── Node identity ───────────────────────────────────────────────────────────
//
// One answer to "which copy of the application is this", shared by everything
// that has to tell copies apart: the leader of a background role, a gateway's
// node registry, a station server's connection owner.
//
// Before it every consumer invented its own: one read an environment variable
// of its own name, another used a bare pid — which says nothing once two
// containers run on one database, where pids repeat.
//
// Resolution order:
//   1. @p configured — the "node" key of the configuration, if not empty;
//   2. $NODE_NAME     — in Kubernetes, the downward API's pod name;
//   3. the host name  — a container's own, which docker sets to its id.
//
// An empty value at any step falls through to the next: "node": "${NODE_NAME}"
// with the variable unset expands to "" and lands on the host name.
std::string node_id(std::string_view configured = {});

/// node_id() plus ":<pid>" — one process of that copy.
std::string node_process_id(std::string_view configured = {});

} // namespace apostol
