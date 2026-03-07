#pragma once

#ifdef WITH_SSL

#include "apostol/event_loop.hpp"
#include "apostol/tcp_client.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

// ─── SmtpConfig ──────────────────────────────────────────────────────────────

struct SmtpConfig
{
    std::string host;
    uint16_t    port{587};     // 25=plain, 587=STARTTLS, 465=implicit TLS
    std::string username;
    std::string password;
    std::string from;          // envelope sender (defaults to username if empty)
};

// ─── SmtpMessage ─────────────────────────────────────────────────────────────

struct SmtpMessage
{
    std::string              from;
    std::vector<std::string> to;
    std::string              subject;
    std::string              body;
    std::string              content_type{"text/plain"};

    std::function<void(const SmtpMessage&)>                   on_done;
    std::function<void(const SmtpMessage&, std::string_view)> on_error;
};

// ─── SmtpClient ──────────────────────────────────────────────────────────────
//
// Async SMTP client with STARTTLS support.
//
// Usage:
//   SmtpConfig cfg{"smtp.host.com", 587, "user@host.com", "password"};
//   SmtpClient smtp(loop, cfg);
//   auto& msg = smtp.add_message();
//   msg.from = "noreply@host.com";
//   msg.to = {"user@example.com"};
//   msg.subject = "Test";
//   msg.body = "Hello";
//   msg.on_done = [](auto&){ fmt::print("sent\n"); };
//   smtp.send_mail();
//
class SmtpClient
{
public:
    SmtpClient(EventLoop& loop, const SmtpConfig& config);
    ~SmtpClient();

    SmtpClient(const SmtpClient&)            = delete;
    SmtpClient& operator=(const SmtpClient&) = delete;

    /// Add a message to the send queue. Returns reference for populating.
    SmtpMessage& add_message();

    /// Connect to SMTP server and send all queued messages.
    void send_mail();

    bool active() const noexcept { return active_; }

private:
    enum class State {
        Idle,
        Connecting,
        WaitGreeting,
        SentEhlo,
        SentStartTls,
        TlsHandshake,
        SentEhloAfterTls,
        SentAuthLogin,
        SentUsername,
        SentPassword,
        SentMailFrom,
        SentRcptTo,
        SentData,
        SentContent,
        SentQuit,
        Done,
        Error
    };

    void on_data(const char* data, size_t len);
    void process_reply();
    void advance(int code, std::string_view text);
    void send_command(std::string_view cmd);
    void send_next_message();
    void enter_error(std::string_view msg);

    static std::string base64_encode(std::string_view input);
    static std::string build_message_data(const SmtpMessage& msg);

    EventLoop&  loop_;
    SmtpConfig  config_;
    TcpClient   tcp_;
    bool        active_{false};

    State state_{State::Idle};
    std::string reply_buf_;

    std::vector<SmtpMessage> messages_;
    std::size_t current_msg_{0};
    std::size_t current_rcpt_{0};
};

} // namespace apostol

#endif // WITH_SSL
