#pragma once

#include "apostol/http.hpp"
#include "apostol/tcp.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace apostol
{

// ── WebSocket opcodes ─────────────────────────────────────────────────────────

constexpr uint8_t WS_OP_CONTINUATION = 0x0;
constexpr uint8_t WS_OP_TEXT         = 0x1;
constexpr uint8_t WS_OP_BINARY       = 0x2;
constexpr uint8_t WS_OP_CLOSE        = 0x8;
constexpr uint8_t WS_OP_PING         = 0x9;
constexpr uint8_t WS_OP_PONG         = 0xA;

// ── Pure functions ────────────────────────────────────────────────────────────

/// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key.
/// Returns base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
std::string ws_accept_key(std::string_view key);

/// Returns true if @p req is a valid WebSocket Upgrade request
/// (GET + Upgrade: websocket + Sec-WebSocket-Key present).
bool is_ws_upgrade(const HttpRequest& req);

/// Build a WebSocket frame (server → client, no masking).
/// opcode: one of WS_OP_TEXT, WS_OP_BINARY, WS_OP_PING, WS_OP_PONG, WS_OP_CLOSE.
/// fin: true for unfragmented or final fragment.
std::string ws_build_frame(uint8_t opcode, std::string_view payload, bool fin = true);

/// Build a WebSocket frame with client masking (client → server, RFC 6455).
/// Same as ws_build_frame() but with MASK bit set and 4-byte random mask key.
std::string ws_build_client_frame(uint8_t opcode, std::string_view payload, bool fin = true);

// ── WsParser ─────────────────────────────────────────────────────────────────

/// Push-parser for WebSocket frames (RFC 6455).
/// Handles masking (client→server), fragmentation, and control frames.
/// Call set_handler() before feed(). The handler is called for each
/// complete message (data frames) or control frame.
class WsParser
{
public:
    using MessageHandler = std::function<void(uint8_t opcode, std::string payload)>;

    void set_handler(MessageHandler h);

    /// Feed raw bytes into the parser.
    /// Returns false if a protocol error was detected.
    bool feed(const char* data, std::size_t len);

    std::string_view error() const noexcept { return error_; }

private:
    enum class State { Header, ExtLen16, ExtLen64, MaskKey, Payload };

    State       state_{State::Header};
    std::string buf_;

    // Current frame metadata
    bool        fin_{false};
    uint8_t     opcode_{0};
    bool        masked_{false};
    uint64_t    payload_len_{0};
    uint8_t     mask_key_[4]{};
    std::string payload_;

    // Fragmentation reassembly
    uint8_t     frag_opcode_{0};
    std::string frag_payload_;

    std::string    error_;
    MessageHandler handler_;

    bool process();
    bool process_header();
    bool process_ext_len16();
    bool process_ext_len64();
    bool process_mask_key();
    bool process_payload();
    bool deliver_frame();
};

// ── WsConnection ─────────────────────────────────────────────────────────────

/// Owns a TcpConnection after a WebSocket upgrade.
/// Provides framing (send_text, send_binary, send_ping, send_close)
/// and on_readable() which auto-responds to pings with pongs.
class WsConnection
{
public:
    using MessageHandler = std::function<void(uint8_t opcode, const std::string& payload)>;
    using CloseHandler   = std::function<void()>;

    explicit WsConnection(TcpConnection conn);

    int  fd()     const noexcept { return conn_.fd(); }
    bool closed() const noexcept { return closed_; }

    /// Called each time the fd becomes readable.
    /// Delivers messages via @p on_msg, auto-pongs pings.
    /// Returns false when the connection should be closed (close frame or EOF).
    bool on_readable(MessageHandler on_msg, CloseHandler on_close = {});

    void send_text  (std::string_view text);
    void send_binary(std::string_view data);
    void send_ping  (std::string_view data = {});
    void send_close (uint16_t code = 1000, std::string_view reason = {});

private:
    TcpConnection conn_;
    WsParser      parser_;
    bool          closed_{false};

    void send_raw(uint8_t opcode, std::string_view payload, bool fin = true);
};

// ── HTTP → WebSocket upgrade ──────────────────────────────────────────────────

/// If @p req is a WebSocket upgrade, send "101 Switching Protocols",
/// transfer the TCP connection out of @p conn, and return a WsConnection.
/// Returns nullopt if @p req is not a WebSocket upgrade request.
/// After this call @p conn must not be used again.
std::optional<WsConnection> ws_upgrade(HttpConnection& conn, const HttpRequest& req);

} // namespace apostol
