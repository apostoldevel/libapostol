#include "apostol/ws_client.hpp"
#include "apostol/base64.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <random>

namespace apostol
{

// ── WsMessage ────────────────────────────────────────────────────────────────

std::string WsMessage::to_json() const
{
    nlohmann::json j;
    if (!id.empty())        j["id"]      = id;
    if (!action.empty())    j["action"]  = action;
    if (!payload.is_null()) j["payload"] = payload;

    if (type == Type::Error) {
        j["type"] = "error";
        if (!error_code.empty())        j["error_code"]        = error_code;
        if (!error_description.empty()) j["error_description"] = error_description;
    } else if (type == Type::Response) {
        j["type"] = "response";
    }
    // Type::Request is the default — omit for backward compatibility

    return j.dump();
}

WsMessage WsMessage::from_json(std::string_view text)
{
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return {};

    WsMessage msg;
    if (j.contains("id") && j["id"].is_string())
        msg.id = j["id"].get<std::string>();
    if (j.contains("action") && j["action"].is_string())
        msg.action = j["action"].get<std::string>();
    if (j.contains("payload"))
        msg.payload = j["payload"];

    if (j.contains("type") && j["type"].is_string()) {
        auto t = j["type"].get<std::string>();
        if (t == "response")  msg.type = Type::Response;
        else if (t == "error") {
            msg.type = Type::Error;
            if (j.contains("error_code") && j["error_code"].is_string())
                msg.error_code = j["error_code"].get<std::string>();
            if (j.contains("error_description") && j["error_description"].is_string())
                msg.error_description = j["error_description"].get<std::string>();
        }
    }

    return msg;
}

// ── URL parsing ──────────────────────────────────────────────────────────────

WsClient::ParsedUrl WsClient::parse_url(std::string_view url)
{
    ParsedUrl result;

    auto pos = url.find("://");
    if (pos == std::string_view::npos) return result;
    result.scheme = std::string(url.substr(0, pos));
    url.remove_prefix(pos + 3);

    auto slash = url.find('/');
    auto host_port = (slash != std::string_view::npos) ? url.substr(0, slash) : url;

    auto colon = host_port.rfind(':');
    if (colon != std::string_view::npos) {
        result.host = std::string(host_port.substr(0, colon));
        auto port_sv = host_port.substr(colon + 1);
        int port_val = 0;
        auto [ptr, ec] = std::from_chars(port_sv.data(),
                                          port_sv.data() + port_sv.size(),
                                          port_val);
        result.port = (ec == std::errc{})
            ? static_cast<uint16_t>(port_val) : 0;
    } else {
        result.host = std::string(host_port);
        result.port = (result.scheme == "wss") ? 443 : 80;
    }

    result.path = (slash != std::string_view::npos)
        ? std::string(url.substr(slash)) : "/";
    return result;
}

// ── Key / ID generation ──────────────────────────────────────────────────────

static thread_local std::mt19937 tl_rng{std::random_device{}()};

std::string WsClient::generate_key()
{
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::string raw(16, '\0');
    for (auto& c : raw) c = static_cast<char>(dist(tl_rng));
    return base64_encode(raw);
}

std::string WsClient::generate_id()
{
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::string raw(12, '\0');
    for (auto& c : raw) c = static_cast<char>(dist(tl_rng));
    return base64_encode(raw);
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

WsClient::WsClient(EventLoop& loop)
    : loop_(loop)
    , tcp_(loop)
{}

WsClient::~WsClient()
{
    cleanup();
}

// ── connect ──────────────────────────────────────────────────────────────────

void WsClient::connect(std::string_view url,
                        const std::vector<std::string>& protocols,
                        const std::vector<std::pair<std::string, std::string>>& headers)
{
    if (state_ != WsClientState::Idle && state_ != WsClientState::Closed &&
        state_ != WsClientState::Error && state_ != WsClientState::Reconnecting)
    {
        enter_error("connect() called in invalid state");
        return;
    }

    url_ = parse_url(url);
    if (url_.host.empty()) {
        enter_error("invalid URL");
        return;
    }

    protocols_ = protocols;
    extra_headers_ = headers;
    do_connect();
}

void WsClient::do_connect()
{
    state_ = WsClientState::Connecting;
    upgrading_ = false;
    pong_pending_ = false;
    pong_miss_count_ = 0;
    upgrade_buf_.clear();

#ifdef WITH_SSL
    if (tls_enabled_ || url_.scheme == "wss")
        tcp_.enable_tls(tls_verify_);
#endif

    tcp_.on_connect([this] {
        send_upgrade_request();
    });

    tcp_.on_data([this](const char* data, size_t len) {
        on_tcp_data(data, len);
    });

    tcp_.on_close([this] {
        if (state_ == WsClientState::Closing || state_ == WsClientState::Closed ||
            state_ == WsClientState::Reconnecting)
            return;

        cancel_ping_timer();

        if (auto_reconnect_ &&
            (state_ == WsClientState::Connected || state_ == WsClientState::Error))
        {
            // Notify caller before reconnecting so they can log the reason
            if (on_close_) on_close_(1006, "connection lost");
            state_ = WsClientState::Reconnecting;
            start_reconnect_timer();
        } else {
            state_ = WsClientState::Closed;
            if (on_close_) on_close_(1006, "connection lost");
        }
    });

    tcp_.on_error([this](std::string_view err) {
        cancel_ping_timer();

        if (auto_reconnect_ && (state_ == WsClientState::Connected ||
                                state_ == WsClientState::Connecting ||
                                state_ == WsClientState::Upgrading))
        {
            // Notify caller before reconnecting so they can log the reason
            if (on_error_) on_error_(err);
            state_ = WsClientState::Reconnecting;
            start_reconnect_timer();
        } else {
            enter_error(err);
        }
    });

    tcp_.connect(url_.host, url_.port);
}

// ── Upgrade handshake ────────────────────────────────────────────────────────

void WsClient::send_upgrade_request()
{
    state_ = WsClientState::Upgrading;
    upgrading_ = true;
    ws_key_ = generate_key();

    std::string request = fmt::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}{}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n",
        url_.path,
        url_.host,
        (url_.port != 80 && url_.port != 443)
            ? fmt::format(":{}", url_.port) : "",
        ws_key_);

    if (!protocols_.empty()) {
        std::string proto_list;
        for (std::size_t i = 0; i < protocols_.size(); ++i) {
            if (i > 0) proto_list += ", ";
            proto_list += protocols_[i];
        }
        request += fmt::format("Sec-WebSocket-Protocol: {}\r\n", proto_list);
    }

    for (const auto& [name, value] : extra_headers_)
        request += fmt::format("{}: {}\r\n", name, value);

    request += "\r\n";
    tcp_.send(request);
}

// ── Data routing ─────────────────────────────────────────────────────────────

void WsClient::on_tcp_data(const char* data, std::size_t len)
{
    if (upgrading_) {
        upgrade_buf_.append(data, len);

        // Guard against unbounded buffering from malicious server
        constexpr std::size_t kMaxUpgradeResponse = 8192;
        if (upgrade_buf_.size() > kMaxUpgradeResponse &&
            upgrade_buf_.find("\r\n\r\n") == std::string::npos)
        {
            enter_error("upgrade response too large");
            return;
        }

        auto end = upgrade_buf_.find("\r\n\r\n");
        if (end == std::string::npos) return;  // incomplete headers

        // Parse status line
        auto line_end = upgrade_buf_.find("\r\n");
        auto status_line = std::string_view(upgrade_buf_).substr(0, line_end);

        auto sp1 = status_line.find(' ');
        if (sp1 == std::string_view::npos) {
            enter_error("invalid upgrade response");
            return;
        }
        auto sp2 = status_line.find(' ', sp1 + 1);
        auto code_sv = status_line.substr(sp1 + 1,
            (sp2 != std::string_view::npos) ? sp2 - sp1 - 1 : std::string_view::npos);
        int code = 0;
        std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), code);

        if (code != 101) {
            enter_error(fmt::format("upgrade failed: {}", status_line));
            return;
        }

        // Parse headers (case-insensitive)
        std::unordered_map<std::string, std::string> headers;
        auto hdr_block = std::string_view(upgrade_buf_).substr(line_end + 2,
                                                                end - line_end - 2);
        while (!hdr_block.empty()) {
            auto le = hdr_block.find("\r\n");
            auto line = (le != std::string_view::npos)
                ? hdr_block.substr(0, le) : hdr_block;
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string name(line.substr(0, colon));
                std::transform(name.begin(), name.end(), name.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                auto val = line.substr(colon + 1);
                auto start = val.find_first_not_of(' ');
                headers[name] = (start != std::string_view::npos)
                    ? std::string(val.substr(start)) : "";
            }

            if (le == std::string_view::npos) break;
            hdr_block.remove_prefix(le + 2);
        }

        // Validate Sec-WebSocket-Accept
        auto it = headers.find("sec-websocket-accept");
        if (it == headers.end() || it->second != ws_accept_key(ws_key_)) {
            enter_error("invalid Sec-WebSocket-Accept");
            return;
        }

        on_upgrade_complete();

        // Feed leftover bytes (WS frames that arrived with the 101)
        std::size_t ws_start = end + 4;
        if (ws_start < upgrade_buf_.size())
            ws_parser_.feed(upgrade_buf_.data() + ws_start,
                            upgrade_buf_.size() - ws_start);

        upgrade_buf_.clear();
        return;
    }

    if (state_ == WsClientState::Connected || state_ == WsClientState::Closing)
        ws_parser_.feed(data, len);
}

void WsClient::on_upgrade_complete()
{
    upgrading_ = false;
    state_ = WsClientState::Connected;
    reconnect_delay_ = std::chrono::seconds(1);  // reset backoff

    ws_parser_ = WsParser{};
    ws_parser_.set_handler([this](uint8_t opcode, std::string payload) {
        on_ws_message(opcode, std::move(payload));
    });

    start_ping_timer();
    if (on_connect_) on_connect_();
}

// ── WebSocket message handling ───────────────────────────────────────────────

void WsClient::on_ws_message(uint8_t opcode, std::string payload)
{
    switch (opcode) {
    case WS_OP_TEXT:
        handle_text_message(std::move(payload));
        break;
    case WS_OP_BINARY:
        if (on_message_) on_message_(opcode, std::move(payload));
        break;
    case WS_OP_PING:
        // RFC 6455: MUST respond with pong echoing the data
        tcp_.send(ws_build_client_frame(WS_OP_PONG, payload));
        break;
    case WS_OP_PONG:
        pong_pending_ = false;
        break;
    case WS_OP_CLOSE:
        handle_close_frame(payload);
        break;
    default:
        if (on_message_) on_message_(opcode, std::move(payload));
        break;
    }
}

void WsClient::handle_text_message(std::string payload)
{
    auto msg = codec_ ? codec_->deserialize(payload)
                      : WsMessage::from_json(payload);

    // Check correlation (request/response)
    if (!msg.id.empty()) {
        auto it = pending_responses_.find(msg.id);
        if (it != pending_responses_.end()) {
            auto handler = std::move(it->second.handler);
            loop_.cancel_timer(it->second.timer);
            pending_responses_.erase(it);
            if (handler) handler(msg);
            return;
        }
    }

    // Check action dispatch
    if (!msg.action.empty()) {
        auto it = action_handlers_.find(msg.action);
        if (it != action_handlers_.end()) {
            WsMessage response;
            response.id = msg.id;
            it->second(*this, msg, response);
            if (!response.action.empty() || !response.payload.is_null()) {
                auto text = codec_ ? codec_->serialize(response)
                                   : response.to_json();
                send_text(text);
            }
            return;
        }
    }

    // Fallback to generic message handler
    if (on_message_) on_message_(WS_OP_TEXT, std::move(payload));
}

void WsClient::handle_close_frame(std::string_view payload)
{
    uint16_t code = 1000;
    std::string_view reason;

    if (payload.size() >= 2) {
        code = static_cast<uint16_t>(
            (static_cast<uint8_t>(payload[0]) << 8) |
             static_cast<uint8_t>(payload[1]));
        reason = payload.substr(2);
    }

    bool we_initiated = (state_ == WsClientState::Closing);

    // If server initiated, echo close frame
    if (!we_initiated) {
        std::string close_payload;
        close_payload.push_back(static_cast<char>(code >> 8));
        close_payload.push_back(static_cast<char>(code & 0xFF));
        tcp_.send(ws_build_client_frame(WS_OP_CLOSE, close_payload));
    }

    cancel_ping_timer();
    state_ = WsClientState::Closed;

    if (on_close_) on_close_(code, reason);

    // Auto-reconnect only if server initiated close
    if (auto_reconnect_ && !we_initiated) {
        state_ = WsClientState::Reconnecting;
        start_reconnect_timer();
    }
}

// ── Sending ──────────────────────────────────────────────────────────────────

void WsClient::send_text(std::string_view text)
{
    if (state_ != WsClientState::Connected) return;
    tcp_.send(ws_build_client_frame(WS_OP_TEXT, text));
}

void WsClient::send_binary(std::string_view data)
{
    if (state_ != WsClientState::Connected) return;
    tcp_.send(ws_build_client_frame(WS_OP_BINARY, data));
}

void WsClient::send_ping(std::string_view data)
{
    if (state_ != WsClientState::Connected) return;
    tcp_.send(ws_build_client_frame(WS_OP_PING, data));
}

void WsClient::send(const WsMessage& message, ResponseHandler on_response,
                     std::chrono::milliseconds timeout)
{
    if (state_ != WsClientState::Connected) return;

    WsMessage msg = message;
    if (msg.id.empty())
        msg.id = generate_id();

    if (on_response) {
        auto timer = loop_.add_timer(timeout, [this, id = msg.id] {
            auto it = pending_responses_.find(id);
            if (it != pending_responses_.end()) {
                pending_responses_.erase(it);
                if (on_error_)
                    on_error_(fmt::format("response timeout for {}", id));
            }
        }, false);

        pending_responses_[msg.id] = {std::move(on_response), timer};
    }

    auto text = codec_ ? codec_->serialize(msg) : msg.to_json();
    send_text(text);
}

// ── Close ────────────────────────────────────────────────────────────────────

void WsClient::close(uint16_t code, std::string_view reason)
{
    if (state_ != WsClientState::Connected) return;

    state_ = WsClientState::Closing;
    cancel_ping_timer();

    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), reason.size());
    tcp_.send(ws_build_client_frame(WS_OP_CLOSE, payload));
}

void WsClient::reconnect()
{
    if (state_ == WsClientState::Reconnecting)
        return;

    cancel_ping_timer();
    state_ = WsClientState::Reconnecting;
    reconnect_delay_ = std::chrono::seconds(1);
    tcp_.close();
    start_reconnect_timer();
}

// ── Action handlers ──────────────────────────────────────────────────────────

void WsClient::on_action(std::string_view action, ActionHandler handler)
{
    action_handlers_[std::string(action)] = std::move(handler);
}

// ── Settings ─────────────────────────────────────────────────────────────────

void WsClient::set_connect_timeout(std::chrono::milliseconds ms)
{
    tcp_.set_connect_timeout(ms);
}

void WsClient::set_codec(std::unique_ptr<WsCodec> codec)
{
    codec_ = std::move(codec);
}

#ifdef WITH_SSL
void WsClient::enable_tls(bool verify)
{
    tls_enabled_ = true;
    tls_verify_ = verify;
}
#endif

// ── Ping/pong timer ──────────────────────────────────────────────────────────

void WsClient::start_ping_timer()
{
    cancel_ping_timer();
    if (ping_interval_.count() == 0) return;

    ping_timer_ = loop_.add_timer(
        std::chrono::duration_cast<std::chrono::milliseconds>(ping_interval_),
        [this] {
            if (state_ != WsClientState::Connected) return;

            if (pong_pending_) {
                ++pong_miss_count_;
                if (pong_miss_count_ >= 2) {
                    // Pong timeout — force reconnect instead of entering
                    // dead Error state (tcp_.on_close won't reconnect from Error).
                    cancel_ping_timer();
                    if (auto_reconnect_) {
                        state_ = WsClientState::Reconnecting;
                        tcp_.close();
                        start_reconnect_timer();
                    } else {
                        enter_error("pong timeout");
                    }
                    return;
                }
            } else {
                pong_miss_count_ = 0;
            }

            pong_pending_ = true;
            send_ping();
        },
        true);
}

void WsClient::cancel_ping_timer()
{
    if (ping_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(ping_timer_);
        ping_timer_ = EventLoop::kInvalidTimer;
    }
}

// ── Reconnect ────────────────────────────────────────────────────────────────

void WsClient::start_reconnect_timer()
{
    cancel_reconnect_timer();

    reconnect_timer_ = loop_.add_timer(
        std::chrono::duration_cast<std::chrono::milliseconds>(reconnect_delay_),
        [this] {
            reconnect_timer_ = EventLoop::kInvalidTimer;
            do_reconnect();
        },
        false);

    // Exponential backoff
    reconnect_delay_ = std::min(reconnect_delay_ * 2, reconnect_max_delay_);
}

void WsClient::cancel_reconnect_timer()
{
    if (reconnect_timer_ != EventLoop::kInvalidTimer) {
        loop_.cancel_timer(reconnect_timer_);
        reconnect_timer_ = EventLoop::kInvalidTimer;
    }
}

void WsClient::do_reconnect()
{
    on_before_reconnect();

    // Clear pending responses (cannot survive reconnect)
    for (auto& [id, pending] : pending_responses_)
        loop_.cancel_timer(pending.timer);
    pending_responses_.clear();

    do_connect();
}

// ── Error / cleanup ──────────────────────────────────────────────────────────

void WsClient::enter_error(std::string_view msg)
{
    cancel_ping_timer();
    cancel_reconnect_timer();
    state_ = WsClientState::Error;
    if (on_error_) on_error_(msg);
}

void WsClient::cleanup()
{
    cancel_ping_timer();
    cancel_reconnect_timer();

    for (auto& [id, pending] : pending_responses_)
        loop_.cancel_timer(pending.timer);
    pending_responses_.clear();
}

} // namespace apostol
