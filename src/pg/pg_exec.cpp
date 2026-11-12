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
    // A NUL byte cannot be sent: libpq takes the statement as a C string and
    // would send only what comes before it, and no text value can hold one.
    // The statement is built from the request, so the request is refused,
    // before the database sees any of it.
    if (sql.find('\0') != std::string::npos) {
        reply_error(resp, HttpStatus::bad_request,
                    "the request contains a NUL byte");
        return;
    }

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
