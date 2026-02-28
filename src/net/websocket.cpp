#include "apostol/websocket.hpp"

#ifdef WITH_SSL
#  include <openssl/bio.h>
#  include <openssl/buffer.h>
#  include <openssl/evp.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace apostol
{

// ── ws_accept_key helpers (no-SSL fallback) ───────────────────────────────────

#ifndef WITH_SSL

// RFC 3174 SHA-1 — pure C++, no dependencies
static std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len)
{
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u,
             h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    // Pre-processing: pad message
    std::size_t total  = len;
    std::size_t padded = ((total + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    std::memcpy(msg.data(), data, total);
    msg[total] = 0x80;
    uint64_t bits = static_cast<uint64_t>(total) * 8;
    for (int i = 0; i < 8; ++i)
        msg[padded - 8 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));

    auto rol = [](uint32_t v, int n) -> uint32_t {
        return (v << n) | (v >> (32 - n));
    };

    for (std::size_t i = 0; i < padded; i += 64)
    {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = (uint32_t(msg[i+j*4])<<24) | (uint32_t(msg[i+j*4+1])<<16)
                 | (uint32_t(msg[i+j*4+2])<<8)| uint32_t(msg[i+j*4+3]);
        for (int j = 16; j < 80; ++j)
            w[j] = rol(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);

        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int j = 0; j < 80; ++j)
        {
            uint32_t f, k;
            if      (j < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (j < 40) { f =  b ^ c ^ d;          k = 0x6ED9EBA1u; }
            else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f =  b ^ c ^ d;          k = 0xCA62C1D6u; }
            uint32_t tmp = rol(a,5) + f + e + k + w[j];
            e=d; d=c; c=rol(b,30); b=a; a=tmp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }

    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 4; ++i)
    {
        out[i]    = (h0 >> (24 - i*8)) & 0xff;
        out[4+i]  = (h1 >> (24 - i*8)) & 0xff;
        out[8+i]  = (h2 >> (24 - i*8)) & 0xff;
        out[12+i] = (h3 >> (24 - i*8)) & 0xff;
        out[16+i] = (h4 >> (24 - i*8)) & 0xff;
    }
    return out;
}

// Standard Base64 encoder
static std::string base64_encode(const uint8_t* data, std::size_t len)
{
    static constexpr char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3)
    {
        uint32_t v = (uint32_t(data[i]) << 16)
                   | (i+1 < len ? uint32_t(data[i+1]) << 8 : 0)
                   | (i+2 < len ? uint32_t(data[i+2])      : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += (i+1 < len) ? t[(v >> 6) & 63] : '=';
        out += (i+2 < len) ? t[ v       & 63] : '=';
    }
    return out;
}

#endif // !WITH_SSL

// ── ws_accept_key ─────────────────────────────────────────────────────────────

std::string ws_accept_key(std::string_view key)
{
    // Concatenate with the RFC 6455 magic GUID
    std::string combined(key);
    combined += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#ifdef WITH_SSL
    // SHA-1 + Base64 via OpenSSL
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    EVP_Digest(combined.data(), combined.size(),
               digest, &digest_len, EVP_sha1(), nullptr);

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, digest, static_cast<int>(digest_len));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
#else
    // SHA-1 + Base64 — pure C++ fallback (no OpenSSL required)
    auto digest = sha1(reinterpret_cast<const uint8_t*>(combined.data()),
                       combined.size());
    return base64_encode(digest.data(), digest.size());
#endif
}

// ── is_ws_upgrade ─────────────────────────────────────────────────────────────

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool is_ws_upgrade(const HttpRequest& req)
{
    if (to_lower(req.method) != "get")
        return false;
    if (to_lower(req.header("Upgrade")) != "websocket")
        return false;
    if (req.header("Sec-WebSocket-Key").empty())
        return false;
    return true;
}

// ── ws_build_frame ────────────────────────────────────────────────────────────

std::string ws_build_frame(uint8_t opcode, std::string_view payload, bool fin)
{
    std::string frame;

    // Byte 0: FIN | opcode
    frame.push_back(static_cast<char>((fin ? 0x80u : 0x00u) | opcode));

    // Byte 1: MASK=0 | 7-bit length (server frames are not masked)
    auto len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len < 65536) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len        & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }

    frame.append(payload.data(), payload.size());
    return frame;
}

// ── ws_build_client_frame ────────────────────────────────────────────────────

std::string ws_build_client_frame(uint8_t opcode, std::string_view payload, bool fin)
{
    std::string frame;

    // Byte 0: FIN | opcode
    frame.push_back(static_cast<char>((fin ? 0x80u : 0x00u) | opcode));

    // Byte 1: MASK=1 | 7-bit length
    auto len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80u | len));
    } else if (len < 65536) {
        frame.push_back(static_cast<char>(0x80u | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len        & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80u | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }

    // 4-byte random mask key
    static thread_local std::mt19937 rng{std::random_device{}()};
    uint32_t r = rng();
    uint8_t mask[4] = {
        static_cast<uint8_t>((r >> 24) & 0xFF),
        static_cast<uint8_t>((r >> 16) & 0xFF),
        static_cast<uint8_t>((r >>  8) & 0xFF),
        static_cast<uint8_t>( r        & 0xFF),
    };
    for (int i = 0; i < 4; ++i)
        frame.push_back(static_cast<char>(mask[i]));

    // XOR payload with mask
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(
            static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));

    return frame;
}

// ── WsParser ──────────────────────────────────────────────────────────────────

void WsParser::set_handler(MessageHandler h)
{
    handler_ = std::move(h);
}

bool WsParser::feed(const char* data, std::size_t len)
{
    buf_.append(data, len);
    return process();
}

bool WsParser::process()
{
    for (;;) {
        bool progress = false;
        switch (state_) {
            case State::Header:   progress = process_header();   break;
            case State::ExtLen16: progress = process_ext_len16(); break;
            case State::ExtLen64: progress = process_ext_len64(); break;
            case State::MaskKey:  progress = process_mask_key();  break;
            case State::Payload:  progress = process_payload();   break;
        }
        if (!progress || !error_.empty())
            break;
    }
    return error_.empty();
}

bool WsParser::process_header()
{
    if (buf_.size() < 2) return false;

    fin_         = (static_cast<uint8_t>(buf_[0]) & 0x80) != 0;
    opcode_      =  static_cast<uint8_t>(buf_[0]) & 0x0F;
    masked_      = (static_cast<uint8_t>(buf_[1]) & 0x80) != 0;
    uint8_t len7 =  static_cast<uint8_t>(buf_[1]) & 0x7F;

    buf_.erase(0, 2);
    payload_.clear();

    if (len7 < 126) {
        payload_len_ = len7;
        state_ = masked_ ? State::MaskKey : State::Payload;
    } else if (len7 == 126) {
        state_ = State::ExtLen16;
    } else {
        state_ = State::ExtLen64;
    }
    return true;
}

bool WsParser::process_ext_len16()
{
    if (buf_.size() < 2) return false;

    payload_len_ = (static_cast<uint8_t>(buf_[0]) << 8)
                 |  static_cast<uint8_t>(buf_[1]);
    buf_.erase(0, 2);
    state_ = masked_ ? State::MaskKey : State::Payload;
    return true;
}

bool WsParser::process_ext_len64()
{
    if (buf_.size() < 8) return false;

    payload_len_ = 0;
    for (int i = 0; i < 8; ++i)
        payload_len_ = (payload_len_ << 8) | static_cast<uint8_t>(buf_[i]);
    buf_.erase(0, 8);
    state_ = masked_ ? State::MaskKey : State::Payload;
    return true;
}

bool WsParser::process_mask_key()
{
    if (buf_.size() < 4) return false;

    for (int i = 0; i < 4; ++i)
        mask_key_[i] = static_cast<uint8_t>(buf_[i]);
    buf_.erase(0, 4);
    state_ = State::Payload;
    return true;
}

bool WsParser::process_payload()
{
    auto need = static_cast<std::size_t>(payload_len_);
    if (buf_.size() < need) return false;

    payload_.assign(buf_.data(), need);
    buf_.erase(0, need);
    state_ = State::Header;   // ready for next frame
    return deliver_frame();
}

bool WsParser::deliver_frame()
{
    // Unmask if needed
    if (masked_) {
        for (std::size_t i = 0; i < payload_.size(); ++i)
            payload_[i] = static_cast<char>(
                static_cast<uint8_t>(payload_[i]) ^ mask_key_[i % 4]);
    }

    // Control frames (ping, pong, close) are never fragmented — deliver directly
    if (opcode_ >= 0x8) {
        if (handler_)
            handler_(opcode_, std::move(payload_));
        payload_.clear();
        return true;
    }

    // Data frames: handle fragmentation
    if (opcode_ != WS_OP_CONTINUATION) {
        // First (or only) fragment
        frag_opcode_  = opcode_;
        frag_payload_ = std::move(payload_);
    } else {
        // Continuation
        frag_payload_ += std::move(payload_);
    }

    if (fin_) {
        // Final fragment — deliver complete message
        if (handler_)
            handler_(frag_opcode_, std::move(frag_payload_));
        frag_payload_.clear();
    }

    payload_.clear();
    return true;
}

// ── WsConnection ──────────────────────────────────────────────────────────────

WsConnection::WsConnection(TcpConnection conn) : conn_(std::move(conn)) {}

void WsConnection::send_raw(uint8_t opcode, std::string_view payload, bool fin)
{
    auto frame = ws_build_frame(opcode, payload, fin);
    const char* ptr = frame.data();
    std::size_t rem = frame.size();
    while (rem > 0) {
        ssize_t n = conn_.write(ptr, rem);
        if (n < 0) continue;   // EAGAIN on loopback — spin
        if (n == 0) break;
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }
}

void WsConnection::send_text(std::string_view text)    { send_raw(WS_OP_TEXT,   text); }
void WsConnection::send_binary(std::string_view data)  { send_raw(WS_OP_BINARY, data); }
void WsConnection::send_ping(std::string_view data)    { send_raw(WS_OP_PING,   data); }

void WsConnection::send_close(uint16_t code, std::string_view reason)
{
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), reason.size());
    send_raw(WS_OP_CLOSE, payload);
}

bool WsConnection::on_readable(MessageHandler on_msg, CloseHandler on_close)
{
    if (closed_) return false;

    bool ping_received  = false;
    bool close_received = false;
    std::string ping_data;

    parser_.set_handler([&](uint8_t opcode, std::string payload) {
        switch (opcode) {
            case WS_OP_PING:
                ping_received = true;
                ping_data = std::move(payload);
                break;
            case WS_OP_CLOSE:
                close_received = true;
                break;
            default:
                if (on_msg) on_msg(opcode, payload);
                break;
        }
    });

    // Drain all available data
    char    buf[4096];
    bool    got_eof = false;
    for (;;) {
        ssize_t n = conn_.read(buf, sizeof(buf));
        if (n < 0) break;                                // EAGAIN
        if (n == 0) { got_eof = true; break; }           // EOF
        if (!parser_.feed(buf, static_cast<std::size_t>(n))) break;
    }

    // Auto-respond to ping
    if (ping_received)
        send_raw(WS_OP_PONG, ping_data);

    if (close_received) {
        send_close(1000);
        closed_ = true;
        if (on_close) on_close();
        return false;
    }

    return !got_eof;
}

// ── ws_upgrade ────────────────────────────────────────────────────────────────

std::optional<WsConnection> ws_upgrade(HttpConnection& conn, const HttpRequest& req)
{
    if (!is_ws_upgrade(req))
        return std::nullopt;

    const auto key    = req.header("Sec-WebSocket-Key");
    const auto accept = ws_accept_key(key);

    // First sub-protocol offered (trim leading spaces)
    const auto proto_hdr = req.header("Sec-WebSocket-Protocol");
    std::string chosen;
    if (!proto_hdr.empty()) {
        auto comma = proto_hdr.find(',');
        chosen = proto_hdr.substr(0, comma);
        auto start = chosen.find_first_not_of(' ');
        if (start != std::string::npos)
            chosen = chosen.substr(start);
    }

    // Build 101 Switching Protocols response
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n";
    if (!chosen.empty())
        response += "Sec-WebSocket-Protocol: " + chosen + "\r\n";
    response += "\r\n";

    // Transfer TCP ownership, send 101, return WsConnection
    auto tcp = conn.release_tcp();
    const char* ptr = response.data();
    std::size_t rem = response.size();
    while (rem > 0) {
        ssize_t n = tcp.write(ptr, rem);
        if (n < 0) continue;
        if (n == 0) break;
        ptr += n;
        rem -= static_cast<std::size_t>(n);
    }
    return WsConnection(std::move(tcp));
}

} // namespace apostol
