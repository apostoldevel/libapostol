#ifdef WITH_POSTGRESQL

#include "apostol/pg_exec.hpp"
#include "apostol/http_utils.hpp"

#include <memory>
#include <utility>

namespace apostol
{

void exec_sql(PgPool& pool, const HttpRequest& req, HttpResponse& resp,
              std::string sql, PgResultHandler on_result, bool quiet)
{
    resp.set_deferred(true);

    auto conn = std::static_pointer_cast<HttpConnection>(req.connection_ctx);

    pool.execute(std::move(sql),
        // on_result
        [conn, handler = std::move(on_result)](std::vector<PgResult> results) {
            handler(conn, std::move(results));
        },
        // on_exception — shared pattern: 500 JSON error
        [conn](std::string_view error) {
            HttpResponse r;
            reply_error(r, HttpStatus::internal_server_error, error);
            conn->send_response(r);
        },
        quiet);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
