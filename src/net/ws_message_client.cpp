#include "apostol/ws_message_client.hpp"
#include "apostol/websocket.hpp"

#include <fmt/format.h>

namespace apostol
{

// ── WsMessage ────────────────────────────────────────────────────────────────

std::string WsMessage::to_json() const
{
    nlohmann::json j;
    if (!id.empty())        j["id"]      = id;
    if (!action.empty())    j["action"]  = action;
    if (!payload.is_null()) j["payload"] = payload;
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
    return msg;
}

// ── WsMessageClient ──────────────────────────────────────────────────────────

WsMessageClient::WsMessageClient(EventLoop& loop)
    : WsClient(loop)
{
    // Intercept raw messages for dispatch
    WsClient::on_message([this](uint8_t opcode, std::string payload) {
        dispatch(opcode, std::move(payload));
    });
}

WsMessageClient::~WsMessageClient()
{
    cancel_pending_timers();
}

void WsMessageClient::on_before_reconnect()
{
    cancel_pending_timers();
}

void WsMessageClient::cancel_pending_timers()
{
    for (auto& [id, pending] : pending_responses_)
        loop_.cancel_timer(pending.timer);
    pending_responses_.clear();
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

void WsMessageClient::dispatch(uint8_t opcode, std::string payload)
{
    // Only parse TEXT frames as WsMessage
    if (opcode != WS_OP_TEXT) {
        if (on_unmatched_) on_unmatched_(opcode, std::move(payload));
        return;
    }

    auto msg = WsMessage::from_json(payload);

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
            if (!response.action.empty() || !response.payload.is_null())
                send_text(response.to_json());
            return;
        }
    }

    // Fallback
    if (on_unmatched_) on_unmatched_(WS_OP_TEXT, std::move(payload));
}

// ── Action registration ──────────────────────────────────────────────────────

void WsMessageClient::on_action(std::string_view action, ActionHandler handler)
{
    action_handlers_[std::string(action)] = std::move(handler);
}

// ── Message-level send ───────────────────────────────────────────────────────

void WsMessageClient::send(const WsMessage& message, ResponseHandler on_response,
                             std::chrono::milliseconds timeout)
{
    if (state() != WsClientState::Connected) return;

    WsMessage msg = message;
    if (msg.id.empty())
        msg.id = generate_id();

    if (on_response) {
        auto timer = loop_.add_timer(timeout, [this, id = msg.id] {
            auto it = pending_responses_.find(id);
            if (it != pending_responses_.end()) {
                pending_responses_.erase(it);
            }
        }, false);

        pending_responses_[msg.id] = {std::move(on_response), timer};
    }

    send_text(msg.to_json());
}

} // namespace apostol
