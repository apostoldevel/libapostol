#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/http.hpp"
#include "apostol/pg.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apostol
{

/// Callback invoked with the captured HttpConnection and PG results.
using PgResultHandler = std::function<void(
    std::shared_ptr<HttpConnection> conn, std::vector<PgResult> results)>;

/// Deferred PG dispatch: marks response as deferred, captures connection_ctx,
/// executes @p sql on @p pool. On PG exception, sends 500 JSON error via conn.
///
/// Eliminates the duplicated on_exception lambda pattern across PGHTTP/FileServer.
void exec_sql(PgPool& pool, const HttpRequest& req, HttpResponse& resp,
              std::string sql, PgResultHandler on_result, bool quiet = false);

} // namespace apostol

#endif // WITH_POSTGRESQL
