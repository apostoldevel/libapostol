#pragma once

#include "apostol/event_loop.hpp"
#include "apostol/tcp_client.hpp"
#include "apostol/websocket.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace apostol
{

// ── WsMessage ────────────────────────────────────────────────────────────────

struct WsMessage
{
    std::string    id;       // correlation ID
    std::string    action;   // action/method name
    nlohmann::json payload;  // JSON payload

    std::string to_json() const;
    static WsMessage from_json(std::string_view text);
};

// ── WsClientState ────────────────────────────────────────────────────────────

enum class WsClientState {
    Idle,
    Connecting,
    Upgrading,
    Connected,
    Closing,
    Reconnecting,
    Closed,
    Error
};

// ── WsClient ─────────────────────────────────────────────────────────────────
//
// Async WebSocket client built on TcpClient.
//
// Usage:
//   WsClient ws(loop);
//   ws.on_connect([] { fmt::print("connected!\n"); });
//   ws.on_message([](uint8_t op, std::string data) { /* ... */ });
//   ws.auto_reconnect(true);
//   ws.connect("ws://127.0.0.1:8080/ws");
//
class WsClient
{
public:
    explicit WsClient(EventLoop& loop);
    ~WsClient();

    WsClient(const WsClient&)            = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&)                 = delete;
    WsClient& operator=(WsClient&&)      = delete;

    // ── Connection ───────────────────────────────────────────────────────────

    void connect(std::string_view url,
                 const std::vector<std::string>& protocols = {},
                 const std::vector<std::pair<std::string, std::string>>& headers = {});
    void close(uint16_t code = 1000, std::string_view reason = {});
    bool connected() const noexcept { return state_ == WsClientState::Connected; }
    WsClientState state() const noexcept { return state_; }

    // ── Sending ──────────────────────────────────────────────────────────────

    void send_text(std::string_view text);
    void send_binary(std::string_view data);
    void send_ping(std::string_view data = {});

    using ResponseHandler = std::function<void(const WsMessage& response)>;
    void send(const WsMessage& message, ResponseHandler on_response = {},
              std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // ── Callbacks ────────────────────────────────────────────────────────────

    using MessageHandler = std::function<void(uint8_t opcode, std::string payload)>;
    using CloseHandler   = std::function<void(uint16_t code, std::string_view reason)>;
    using ErrorHandler   = std::function<void(std::string_view error)>;

    void on_connect(std::function<void()> cb)  { on_connect_ = std::move(cb); }
    void on_message(MessageHandler cb)         { on_message_ = std::move(cb); }
    void on_close(CloseHandler cb)             { on_close_   = std::move(cb); }
    void on_error(ErrorHandler cb)             { on_error_   = std::move(cb); }

    using ActionHandler = std::function<void(WsClient& client,
                                             const WsMessage& request,
                                             WsMessage& response)>;
    void on_action(std::string_view action, ActionHandler handler);

    // ── Settings ─────────────────────────────────────────────────────────────

    void auto_reconnect(bool enable) { auto_reconnect_ = enable; }
    void set_reconnect_max_delay(std::chrono::seconds max) { reconnect_max_delay_ = max; }
    void set_ping_interval(std::chrono::seconds interval)  { ping_interval_ = interval; }
    void set_connect_timeout(std::chrono::milliseconds ms);

#ifdef WITH_SSL
    void enable_tls(bool verify = true);
#endif

private:
    struct ParsedUrl {
        std::string scheme;
        std::string host;
        uint16_t    port{0};
        std::string path;
    };

    static ParsedUrl parse_url(std::string_view url);
    static std::string generate_key();
    static std::string generate_id();

    void do_connect();
    void send_upgrade_request();
    void on_upgrade_complete();
    void on_tcp_data(const char* data, std::size_t len);
    void on_ws_message(uint8_t opcode, std::string payload);
    void handle_text_message(std::string payload);
    void handle_close_frame(std::string_view payload);
    void start_ping_timer();
    void cancel_ping_timer();
    void start_reconnect_timer();
    void cancel_reconnect_timer();
    void do_reconnect();
    void enter_error(std::string_view msg);
    void cleanup();

    EventLoop&    loop_;
    TcpClient     tcp_;
    WsParser      ws_parser_;
    WsClientState state_{WsClientState::Idle};

    // Upgrade handshake buffer (manual HTTP parse, avoids llhttp upgrade issue)
    std::string upgrade_buf_;
    bool        upgrading_{false};
    std::string ws_key_;

    // Connection parameters (saved for reconnect)
    ParsedUrl   url_;
    std::vector<std::string> protocols_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;

    // Callbacks
    std::function<void()>          on_connect_;
    MessageHandler                 on_message_;
    CloseHandler                   on_close_;
    ErrorHandler                   on_error_;
    std::unordered_map<std::string, ActionHandler> action_handlers_;

    // Ping/pong
    std::chrono::seconds   ping_interval_{30};
    EventLoop::TimerId     ping_timer_{EventLoop::kInvalidTimer};
    bool                   pong_pending_{false};
    int                    pong_miss_count_{0};

    // Auto-reconnect
    bool                   auto_reconnect_{false};
    std::chrono::seconds   reconnect_delay_{1};
    std::chrono::seconds   reconnect_max_delay_{60};
    EventLoop::TimerId     reconnect_timer_{EventLoop::kInvalidTimer};

    // Request/response correlation
    struct PendingResponse {
        ResponseHandler    handler;
        EventLoop::TimerId timer;
    };
    std::unordered_map<std::string, PendingResponse> pending_responses_;

#ifdef WITH_SSL
    bool tls_enabled_{false};
    bool tls_verify_{true};
#endif
};

} // namespace apostol
