#ifdef WITH_CURL

#include "apostol/curl_client.hpp"
#include "apostol/fetch_policy.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <sys/socket.h>

namespace apostol
{

// ─── Transfer (per-request state) ───────────────────────────────────────────

struct CurlClient::Transfer
{
    CURL*        easy{nullptr};
    curl_slist*  headers_list{nullptr};
    std::string  body;          // accumulated response body
    std::vector<std::pair<std::string, std::string>> response_headers;
    std::string  post_data;     // must outlive the easy handle

    DoneHandler  on_done;
    FailHandler  on_fail;
    WriteHandler on_write;

    // Outbound address filter (T970): a snapshot of the process policy, so a
    // reload mid-transfer does not change the rules under it; null when off.
    std::shared_ptr<const FetchAddressPolicy> policy;
    std::string  refused;       // why the filter refused a connection, if it did

    ~Transfer()
    {
        if (headers_list)
            curl_slist_free_all(headers_list);
        if (easy)
            curl_easy_cleanup(easy);
    }
};

// ─── Construction / destruction ─────────────────────────────────────────────

CurlClient::CurlClient(EventLoop& loop) : loop_(loop)
{
    multi_ = curl_multi_init();
    if (!multi_)
        throw std::runtime_error("curl_multi_init() failed");

    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &CurlClient::socket_callback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &CurlClient::timer_callback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
}

CurlClient::~CurlClient()
{
    if (timer_id_ != EventLoop::kInvalidTimer)
        loop_.cancel_timer(timer_id_);
    if (lost_timer_ != EventLoop::kInvalidTimer)
        loop_.cancel_timer(lost_timer_);

    // Remove any leftover registered fds from the event loop
    for (auto fd : registered_fds_)
        loop_.remove_io(fd);

    if (multi_)
        curl_multi_cleanup(multi_);
}

// ─── perform ────────────────────────────────────────────────────────────────

void CurlClient::perform(std::string_view url,
                          std::string_view method,
                          std::string_view content,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          DoneHandler on_done,
                          FailHandler on_fail,
                          WriteHandler on_write)
{
    auto xfer = std::make_unique<Transfer>();
    xfer->on_done  = std::move(on_done);
    xfer->on_fail  = std::move(on_fail);
    xfer->on_write = std::move(on_write);

    xfer->easy = curl_easy_init();
    if (!xfer->easy) {
        if (xfer->on_fail)
            xfer->on_fail(CURLE_FAILED_INIT, "curl_easy_init() failed");
        return;
    }

    // Store Transfer* for retrieval in callbacks
    curl_easy_setopt(xfer->easy, CURLOPT_PRIVATE, xfer.get());

    // Protocols: HTTP(S) only, for the URL and for every redirect target.
    // libcurl's default also allows gopher/dict/ftp/file/... — an SSRF primitive
    // when the URL comes from the database (http.fetch). Fail closed if refused.
#if LIBCURL_VERSION_NUM >= 0x075500   // 7.85.0: *_PROTOCOLS_STR, bitmask deprecated
    auto proto_rc = curl_easy_setopt(xfer->easy, CURLOPT_PROTOCOLS_STR, "http,https");
    if (proto_rc == CURLE_OK)
        proto_rc = curl_easy_setopt(xfer->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    auto proto_rc = curl_easy_setopt(xfer->easy, CURLOPT_PROTOCOLS,
                                     static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    if (proto_rc == CURLE_OK)
        proto_rc = curl_easy_setopt(xfer->easy, CURLOPT_REDIR_PROTOCOLS,
                                    static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    if (proto_rc != CURLE_OK) {
        if (xfer->on_fail)
            xfer->on_fail(static_cast<int>(proto_rc), curl_easy_strerror(proto_rc));
        return;
    }

    // Outbound address filter: checked for every connection libcurl opens —
    // the first URL, each redirect target, each address a name resolves to.
    if (auto policy = FetchAddressPolicy::process_default(); policy && policy->enabled()) {
        xfer->policy = std::move(policy);
        curl_easy_setopt(xfer->easy, CURLOPT_OPENSOCKETFUNCTION, &CurlClient::open_socket_callback);
        curl_easy_setopt(xfer->easy, CURLOPT_OPENSOCKETDATA, xfer.get());
    }

    // URL
    std::string url_str(url);
    curl_easy_setopt(xfer->easy, CURLOPT_URL, url_str.c_str());

    // Callbacks
    curl_easy_setopt(xfer->easy, CURLOPT_WRITEFUNCTION, &CurlClient::write_callback);
    curl_easy_setopt(xfer->easy, CURLOPT_WRITEDATA, xfer.get());
    curl_easy_setopt(xfer->easy, CURLOPT_HEADERFUNCTION, &CurlClient::header_callback);
    curl_easy_setopt(xfer->easy, CURLOPT_HEADERDATA, xfer.get());

    // SSL + decompression
    curl_easy_setopt(xfer->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(xfer->easy, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
    curl_easy_setopt(xfer->easy, CURLOPT_HTTP_CONTENT_DECODING, 1L);

    // Follow HTTP redirects (S3 pre-signed URLs, CDN 301/302)
    curl_easy_setopt(xfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(xfer->easy, CURLOPT_MAXREDIRS, 5L);

    // Timeout
    if (timeout_ms_ > 0)
        curl_easy_setopt(xfer->easy, CURLOPT_TIMEOUT_MS, timeout_ms_);

    // Proxy
    if (!proxy_.empty()) {
        curl_easy_setopt(xfer->easy, CURLOPT_PROXY, proxy_.c_str());
        curl_easy_setopt(xfer->easy, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    }

    // Custom headers
    for (const auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        xfer->headers_list = curl_slist_append(xfer->headers_list, h.c_str());
    }
    if (xfer->headers_list)
        curl_easy_setopt(xfer->easy, CURLOPT_HTTPHEADER, xfer->headers_list);

    // HTTP method + body
    if (method == "GET" || method.empty()) {
        curl_easy_setopt(xfer->easy, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        xfer->post_data = std::string(content);
        curl_easy_setopt(xfer->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(xfer->post_data.size()));
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDS, xfer->post_data.c_str());
    } else if (method == "PUT") {
        xfer->post_data = std::string(content);
        curl_easy_setopt(xfer->easy, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(xfer->post_data.size()));
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDS, xfer->post_data.c_str());
    } else if (method == "PATCH") {
        xfer->post_data = std::string(content);
        curl_easy_setopt(xfer->easy, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(xfer->post_data.size()));
        curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDS, xfer->post_data.c_str());
    } else if (method == "DELETE") {
        xfer->post_data = std::string(content);
        curl_easy_setopt(xfer->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!xfer->post_data.empty()) {
            curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(xfer->post_data.size()));
            curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDS, xfer->post_data.c_str());
        }
    } else {
        xfer->post_data = std::string(content);
        curl_easy_setopt(xfer->easy, CURLOPT_CUSTOMREQUEST,
                         std::string(method).c_str());
        if (!xfer->post_data.empty()) {
            curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(xfer->post_data.size()));
            curl_easy_setopt(xfer->easy, CURLOPT_POSTFIELDS, xfer->post_data.c_str());
        }
    }

    // Add to multi handle — ownership transfers to curl_multi
    auto code = curl_multi_add_handle(multi_, xfer->easy);
    if (code != CURLM_OK) {
        if (xfer->on_fail)
            xfer->on_fail(static_cast<int>(code),
                          curl_multi_strerror(code));
        return;
    }

    // Release ownership — Transfer will be deleted in check_multi_info()
    xfer.release();
}

// ─── Convenience methods ────────────────────────────────────────────────────

void CurlClient::get(std::string_view url,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      DoneHandler on_done,
                      FailHandler on_fail)
{
    perform(url, "GET", {}, headers, std::move(on_done), std::move(on_fail));
}

void CurlClient::post(std::string_view url,
                       std::string_view content,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       DoneHandler on_done,
                       FailHandler on_fail)
{
    perform(url, "POST", content, headers, std::move(on_done), std::move(on_fail));
}

// ─── curl_multi socket callback ─────────────────────────────────────────────

int CurlClient::socket_callback(CURL* /*easy*/, curl_socket_t s, int what,
                                 void* clientp, void* /*socketp*/)
{
    auto* self = static_cast<CurlClient*>(clientp);

    if (what == CURL_POLL_REMOVE) {
        self->loop_.remove_io(static_cast<int>(s));
        auto& fds = self->registered_fds_;
        fds.erase(std::remove(fds.begin(), fds.end(), s), fds.end());
        // curl let go of a socket reported lost below: its number is free, and
        // the next socket curl opens will likely get it — that one must be
        // registered, and must not get the error meant for this one.
        auto& lost = self->lost_fds_;
        lost.erase(std::remove(lost.begin(), lost.end(), s), lost.end());
        return 0;
    }

    uint32_t events = 0;
    if (what & CURL_POLL_IN)  events |= EPOLLIN;
    if (what & CURL_POLL_OUT) events |= EPOLLOUT;

    // Check if fd is already registered
    auto& fds = self->registered_fds_;
    bool already = std::find(fds.begin(), fds.end(), s) != fds.end();

    if (already) {
        if (!self->loop_.modify_io(static_cast<int>(s), events)) {
            // Closed under curl (T607): the loop has dropped the handler, and
            // curl, which waits on this socket, would wait for good unless the
            // transfer has a timeout. Forget the registration and tell curl
            // the socket failed — from a timer, not from inside this callback.
            fds.erase(std::remove(fds.begin(), fds.end(), s), fds.end());
            self->lost_fds_.push_back(s);
            if (self->lost_timer_ == EventLoop::kInvalidTimer)
                self->lost_timer_ = self->loop_.add_timer(std::chrono::milliseconds(1),
                    [self]() {
                        self->lost_timer_ = EventLoop::kInvalidTimer;
                        auto lost = std::move(self->lost_fds_);
                        self->lost_fds_.clear();
                        int still_running = 0;
                        for (auto fd : lost)
                            curl_multi_socket_action(self->multi_, fd, CURL_CSELECT_ERR, &still_running);
                        self->check_multi_info();
                    },
                    /*repeat=*/false);
        }
    } else if (std::find(self->lost_fds_.begin(), self->lost_fds_.end(), s) != self->lost_fds_.end()) {
        // Already reported as failed (above), the timer has not run yet: not
        // registered again — add_io on a closed number would throw, and from
        // here through libcurl's frames.
    } else {
        fds.push_back(s);
        self->loop_.add_io(static_cast<int>(s), events,
            [self, s](uint32_t ev) {
                int action = 0;
                if (ev & EPOLLIN)  action |= CURL_CSELECT_IN;
                if (ev & EPOLLOUT) action |= CURL_CSELECT_OUT;

                int still_running = 0;
                curl_multi_socket_action(self->multi_, s, action, &still_running);
                self->check_multi_info();

                // Do NOT cancel the multi timer here from `still_running`. That count
                // was read by curl_multi_socket_action ABOVE, before check_multi_info()
                // ran the completion callbacks — and a completion callback may start a
                // new request. AuthServer does exactly that: the token-exchange reply
                // issues the userinfo fetch from inside its own on_done, on this same
                // client. That new easy handle makes curl schedule a fresh kick-off
                // timer via timer_callback, but `still_running` still reads 0 from
                // before the handle existed. Cancelling on it killed the new transfer's
                // only kick, and it hung until the gateway timed out — a 504 with no log
                // line, because the userinfo callback (its first log_) was never reached.
                // curl owns this timer: it calls timer_callback(-1) when no timer is
                // wanted and a positive timeout when one is, so there is nothing to
                // cancel by hand here. And removing the cancel is safe, not a leak: at
                // worst a now-stale one-shot timer fires once, its socket_action finds
                // no work and check_multi_info no messages — repeat=false, so no loop
                // and nothing left armed. Do NOT restore the cancel "just in case": on
                // the stale count it is exactly the hang this commit removes.
                (void)still_running;

                // Under APOSTOL_EPOLL_ET the fd was armed with EPOLLONESHOT
                // and the kernel disabled further delivery. Rearm so libcurl
                // receives subsequent IO events (it manages the mask itself
                // via socket_callback → modify_io when direction changes).
                // rearm_io is a silent no-op if socket_callback has already
                // removed this fd above (CURL_POLL_REMOVE). Under LT this
                // is a no-op.
                self->loop_.rearm_io(static_cast<int>(s));
            });
    }

    return 0;
}

// ─── curl_multi timer callback ──────────────────────────────────────────────

int CurlClient::timer_callback(CURLM* /*multi*/, long timeout_ms, void* clientp)
{
    auto* self = static_cast<CurlClient*>(clientp);

    // Cancel previous timer
    if (self->timer_id_ != EventLoop::kInvalidTimer) {
        self->loop_.cancel_timer(self->timer_id_);
        self->timer_id_ = EventLoop::kInvalidTimer;
    }

    if (timeout_ms < 0)
        return 0; // -1 means remove timer

    // 0 means call socket_action immediately
    auto interval = std::chrono::milliseconds(timeout_ms == 0 ? 1 : timeout_ms);

    self->timer_id_ = self->loop_.add_timer(interval,
        [self]() {
            // Order is load-bearing: clear timer_id_ HERE, before the calls below —
            // never after them. check_multi_info() runs completion callbacks, and a
            // callback may start a new request (AuthServer's nested userinfo fetch),
            // which schedules a fresh timer via timer_callback and stores its new
            // timer_id_. Clearing to kInvalidTimer after these calls would clobber that
            // id: the new timer would stay armed while we believe we hold none, and
            // timer_callback would never cancel it — the same trap the socket handler
            // above was fixed for. Clear our own (now-firing) one-shot id first, then
            // let curl drive whatever comes next.
            self->timer_id_ = EventLoop::kInvalidTimer;
            int still_running = 0;
            curl_multi_socket_action(self->multi_, CURL_SOCKET_TIMEOUT, 0, &still_running);
            self->check_multi_info();
        },
        /*repeat=*/false);

    return 0;
}

// ─── curl_easy write/header callbacks ───────────────────────────────────────

size_t CurlClient::write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* xfer = static_cast<Transfer*>(userdata);
    size_t total = size * nmemb;

    if (xfer->on_write) {
        xfer->on_write(std::string_view(ptr, total));
    }

    xfer->body.append(ptr, total);
    return total;
}

size_t CurlClient::header_callback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* xfer = static_cast<Transfer*>(userdata);
    size_t total = size * nitems;

    // Skip empty lines and status line
    if (total > 2) {
        std::string_view line(buffer, total);
        // Trim trailing \r\n
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);

        auto sep = line.find(": ");
        if (sep != std::string_view::npos) {
            xfer->response_headers.emplace_back(
                std::string(line.substr(0, sep)),
                std::string(line.substr(sep + 2)));
        }
    }

    return total;
}

// ─── open_socket_callback ───────────────────────────────────────────────────

curl_socket_t CurlClient::open_socket_callback(void* clientp, curlsocktype /*purpose*/,
                                               struct curl_sockaddr* address)
{
    auto* xfer = static_cast<Transfer*>(clientp);

    if (xfer->policy) {
        // Called from libcurl's C frames: nothing may escape. A failure here
        // (allocation) refuses the connection — the filter fails closed.
        try {
            // The host of the URL being connected: after a redirect libcurl has
            // already moved the effective URL to the target. Parsed by libcurl's
            // own URL parser, so the host is the one it dials. Unparsable — no
            // host, and only an address entry of fetch.allow can let it through.
            std::string host;
            char* url = nullptr;
            if (curl_easy_getinfo(xfer->easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url) {
                if (CURLU* u = curl_url()) {
                    char* h = nullptr;
                    if (curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK
                        && curl_url_get(u, CURLUPART_HOST, &h, 0) == CURLUE_OK && h) {
                        host = h;
                        curl_free(h);
                    }
                    curl_url_cleanup(u);
                }
            }

            auto why = xfer->policy->check(&address->addr, host);
            if (!why.empty()) {
                xfer->refused = std::move(why);
                return CURL_SOCKET_BAD;     // libcurl tries the next address, then fails
            }
        } catch (...) {
            return CURL_SOCKET_BAD;
        }
        xfer->refused.clear();          // an address got through: an older refusal is not the story
    }

    return ::socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
}

// ─── check_multi_info ───────────────────────────────────────────────────────

void CurlClient::check_multi_info()
{
    CURLMsg* msg;
    int msgs_left;

    while ((msg = curl_multi_info_read(multi_, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL* easy = msg->easy_handle;
        CURLcode code = msg->data.result;

        Transfer* xfer = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &xfer);
        if (!xfer) continue;

        // Remove from multi before invoking callbacks
        curl_multi_remove_handle(multi_, easy);

        if (code == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

            CurlResponse resp;
            resp.status_code = static_cast<int>(http_code);
            resp.body    = std::move(xfer->body);
            resp.headers = std::move(xfer->response_headers);

            if (xfer->on_done)
                xfer->on_done(std::move(resp));
        } else {
            // A refusal by the address filter says so; libcurl itself would
            // only report "Couldn't connect to server".
            if (xfer->on_fail) {
                if (code == CURLE_COULDNT_CONNECT && !xfer->refused.empty())
                    xfer->on_fail(static_cast<int>(code), xfer->refused);
                else
                    xfer->on_fail(static_cast<int>(code), curl_easy_strerror(code));
            }
        }

        delete xfer;
    }
}

} // namespace apostol

#endif // WITH_CURL
