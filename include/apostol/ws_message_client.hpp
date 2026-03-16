#pragma once

#include "apostol/ws_client.hpp"

#include <unordered_map>

#include <nlohmann/json.hpp>

namespace apostol
{

// ── WsMessageClient ──────────────────────────────────────────────────────────
//
// WsClient with built-in JSON-RPC-like dispatch.
// Parses incoming TEXT frames as WsMessage, dispatches by id (correlation)
// and action (registered handlers). Unmatched messages go to on_unmatched.
//
// Usage:
//   WsMessageClient ws(loop);
//   ws.on_action("hello", [](auto& ws, auto& req, auto& resp) { ... });
//   ws.on_unmatched([](uint8_t op, std::string data) { ... });
//   ws.connect("ws://...");
//
class WsMessageClient : public WsClient
{
public:
    explicit WsMessageClient(EventLoop& loop);
    ~WsMessageClient() override;

    // ── Message-level sending ────────────────────────────────────────────────

    using ResponseHandler = std::function<void(const WsMessage& response)>;
    void send(const WsMessage& message, ResponseHandler on_response = {},
              std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // ── Action dispatch ──────────────────────────────────────────────────────

    using ActionHandler = std::function<void(WsMessageClient& client,
                                             const WsMessage& request,
                                             WsMessage& response)>;
    void on_action(std::string_view action, ActionHandler handler);

    // ── Fallback handler ─────────────────────────────────────────────────────
    //
    // Called for messages that don't match any registered action or pending
    // response. Use this instead of on_message() — on_message() is reserved
    // by the dispatch layer.
    //
    void on_unmatched(MessageHandler cb) { on_unmatched_ = std::move(cb); }

protected:
    void on_before_reconnect() override;

private:
    void dispatch(uint8_t opcode, std::string payload);
    void cancel_pending_timers();

    MessageHandler on_unmatched_;
    std::unordered_map<std::string, ActionHandler> action_handlers_;

    struct PendingResponse {
        ResponseHandler    handler;
        EventLoop::TimerId timer;
    };
    std::unordered_map<std::string, PendingResponse> pending_responses_;
};

} // namespace apostol
