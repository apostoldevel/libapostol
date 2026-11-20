#ifdef WITH_SSL

#include "apostol/smtp_client.hpp"
#include "apostol/base64.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <optional>

namespace apostol
{

namespace
{

// RFC 5321 4.5.3.1.5: a reply line is at most 512 octets with its CRLF. A
// line still open past a few times that is no SMTP reply.
constexpr std::size_t kMaxReplyLine = 4096;

// An envelope address goes into a command line between angle brackets: a
// control byte (CR, LF among them), a space or a bracket would end the
// address — and, with CRLF, the command — where the caller did not mean it.
bool envelope_address_ok(std::string_view address)
{
    for (unsigned char c : address)
        if (c < 0x21 || c == 0x7F || c == '<' || c == '>')
            return false;
    return true;
}

} // namespace

// ─── Constructor / Destructor ────────────────────────────────────────────────

SmtpClient::SmtpClient(EventLoop& loop, const SmtpConfig& config)
    : loop_(loop)
    , config_(config)
    , tcp_(loop)
{}

SmtpClient::~SmtpClient() = default;

// ─── Public API ──────────────────────────────────────────────────────────────

SmtpMessage& SmtpClient::add_message()
{
    messages_.emplace_back();
    return messages_.back();
}

void SmtpClient::send_mail()
{
    if (active_ || messages_.empty())
        return;

    // Every envelope is checked before anything is dialled. A refused message
    // sends nothing and leaves the queue; the others go out. Its on_error runs
    // last, after this client is done with itself: the handler may destroy it.
    std::vector<std::pair<SmtpMessage, std::string>> refused;
    for (auto it = messages_.begin(); it != messages_.end();) {
        std::string why;
        if (!envelope_address_ok(it->from))
            why = "invalid envelope sender";
        else if (it->to.empty())
            why = "no envelope recipient";
        else
            for (const auto& to : it->to)
                if (to.empty() || !envelope_address_ok(to)) { why = "invalid envelope recipient"; break; }

        if (why.empty()) { ++it; continue; }
        refused.emplace_back(std::move(*it), std::move(why));
        it = messages_.erase(it);
    }

    if (!messages_.empty())
        start_session();

    for (auto& [msg, why] : refused)
        if (msg.on_error)
            msg.on_error(msg, why);
}

void SmtpClient::start_session()
{
    active_ = true;
    current_msg_ = 0;
    current_rcpt_ = 0;
    state_ = State::Connecting;
    reply_buf_.clear();   // nothing of an earlier session is read as a reply

    // Port 465 = implicit TLS; others = STARTTLS
    if (config_.port == 465)
        tcp_.enable_tls(false);

    tcp_.set_connect_timeout(std::chrono::milliseconds(10000));

    tcp_.on_connect([this] {
        state_ = State::WaitGreeting;
    });

    tcp_.on_data([this](const char* data, size_t len) {
        on_data(data, len);
    });

    tcp_.on_error([this](std::string_view msg) {
        enter_error(msg);
    });

    tcp_.on_close([this] {
        if (state_ != State::Done && state_ != State::Error)
            enter_error("connection closed unexpectedly");
    });

    tcp_.connect(config_.host, config_.port);
}

// ─── Data reception ──────────────────────────────────────────────────────────

void SmtpClient::on_data(const char* data, size_t len)
{
    reply_buf_.append(data, len);
    process_reply();
}

void SmtpClient::process_reply()
{
    // SMTP replies are line-based. A complete reply ends with "code SP text\r\n"
    // Multi-line replies have "code-text\r\n" continuations.
    for (;;) {
        auto crlf = reply_buf_.find("\r\n");
        if (crlf == std::string::npos) {
            if (reply_buf_.size() > kMaxReplyLine)
                enter_error(fmt::format("SMTP reply line longer than {} bytes", kMaxReplyLine));
            return;  // incomplete line
        }

        auto line = std::string_view(reply_buf_).substr(0, crlf);
        if (line.size() < 4) {
            enter_error(fmt::format("malformed SMTP reply: {}", line));
            return;
        }

        // Check if this is a continuation line (code-text vs code SP text)
        char separator = line[3];
        if (separator == '-') {
            // Continuation — consume and wait for final line
            reply_buf_.erase(0, crlf + 2);
            continue;
        }

        // Final line: code SP text
        int code = 0;
        for (int i = 0; i < 3; ++i) {
            if (line[i] < '0' || line[i] > '9') {
                enter_error(fmt::format("invalid SMTP reply code: {}", line));
                return;
            }
            code = code * 10 + (line[i] - '0');
        }

        // A copy: the erase below moves what follows the line over it.
        const std::string text((line.size() > 4) ? line.substr(4) : std::string_view{});
        reply_buf_.erase(0, crlf + 2);

        advance(code, text);
        return;
    }
}

// ─── State machine ───────────────────────────────────────────────────────────

void SmtpClient::advance(int code, std::string_view text)
{
    switch (state_) {
        case State::WaitGreeting:
            if (code != 220) { enter_error(fmt::format("greeting: {} {}", code, text)); return; }
            state_ = State::SentEhlo;
            send_command(fmt::format("EHLO {}", config_.host));
            break;

        case State::SentEhlo:
            if (code != 250) { enter_error(fmt::format("EHLO: {} {}", code, text)); return; }
            // STARTTLS for non-465 ports
            if (config_.port != 465) {
                state_ = State::SentStartTls;
                send_command("STARTTLS");
            } else {
                // Already TLS (port 465) — proceed to auth
                if (!config_.username.empty()) {
                    state_ = State::SentAuthLogin;
                    send_command("AUTH LOGIN");
                } else {
                    send_next_message();
                }
            }
            break;

        case State::SentStartTls:
            if (code != 220) { enter_error(fmt::format("STARTTLS: {} {}", code, text)); return; }
            // Whatever came in plain text after the 220 would be read as the
            // server's answer once TLS is up: nothing may come before the
            // handshake (RFC 3207 5), and a server that sends it is refused.
            if (!reply_buf_.empty()) {
                enter_error("STARTTLS: data after the 220 reply before the TLS handshake");
                return;
            }
            state_ = State::TlsHandshake;
            // TcpClient::start_tls() → TlsHandshake → on_connected() fires
            // on_connect_ again once the handshake is done: EHLO is sent anew
            // over TLS. Set before start_tls(), which may finish the handshake,
            // or fail and report it, before it returns — nothing of this client
            // is touched after it.
            tcp_.on_connect([this] {
                state_ = State::SentEhloAfterTls;
                send_command(fmt::format("EHLO {}", config_.host));
            });
            tcp_.start_tls();
            break;

        case State::SentEhloAfterTls:
            if (code != 250) { enter_error(fmt::format("EHLO after TLS: {} {}", code, text)); return; }
            if (!config_.username.empty()) {
                state_ = State::SentAuthLogin;
                send_command("AUTH LOGIN");
            } else {
                send_next_message();
            }
            break;

        case State::SentAuthLogin:
            if (code != 334) { enter_error(fmt::format("AUTH LOGIN: {} {}", code, text)); return; }
            state_ = State::SentUsername;
            send_command(base64_encode(config_.username));
            break;

        case State::SentUsername:
            if (code != 334) { enter_error(fmt::format("username: {} {}", code, text)); return; }
            state_ = State::SentPassword;
            send_command(base64_encode(config_.password));
            break;

        case State::SentPassword:
            if (code != 235) { enter_error(fmt::format("auth failed: {} {}", code, text)); return; }
            send_next_message();
            break;

        case State::SentMailFrom:
            if (code != 250) { enter_error(fmt::format("MAIL FROM: {} {}", code, text)); return; }
            current_rcpt_ = 0;
            state_ = State::SentRcptTo;
            send_command(fmt::format("RCPT TO:<{}>", messages_[current_msg_].to[current_rcpt_]));
            break;

        case State::SentRcptTo:
            if (code != 250 && code != 251) { enter_error(fmt::format("RCPT TO: {} {}", code, text)); return; }
            ++current_rcpt_;
            if (current_rcpt_ < messages_[current_msg_].to.size()) {
                send_command(fmt::format("RCPT TO:<{}>", messages_[current_msg_].to[current_rcpt_]));
            } else {
                state_ = State::SentData;
                send_command("DATA");
            }
            break;

        case State::SentData:
            if (code != 354) { enter_error(fmt::format("DATA: {} {}", code, text)); return; }
            state_ = State::SentContent;
            {
                // One send: a refused one reports on_error, which may destroy
                // this client — nothing of it is touched after.
                auto content = build_message_data(messages_[current_msg_]);
                content += "\r\n.\r\n";
                tcp_.send(content);
            }
            break;

        case State::SentContent:
            if (code != 250) { enter_error(fmt::format("message send: {} {}", code, text)); return; }
            // Message sent successfully: it leaves its place in the queue for
            // the handler, which may destroy this client — then nothing of it
            // is touched.
            if (messages_[current_msg_].on_done) {
                const std::weak_ptr<int> alive = alive_;
                const SmtpMessage sent = std::move(messages_[current_msg_]);
                sent.on_done(sent);
                if (alive.expired())
                    return;
            }
            ++current_msg_;
            if (current_msg_ < messages_.size()) {
                send_next_message();
            } else {
                state_ = State::SentQuit;
                send_command("QUIT");
            }
            break;

        case State::SentQuit:
            state_ = State::Done;
            active_ = false;
            tcp_.close();
            messages_.clear();
            break;

        default:
            break;
    }
}

void SmtpClient::send_next_message()
{
    if (current_msg_ >= messages_.size()) {
        state_ = State::SentQuit;
        send_command("QUIT");
        return;
    }

    state_ = State::SentMailFrom;
    send_command(fmt::format("MAIL FROM:<{}>", messages_[current_msg_].from));
}

void SmtpClient::send_command(std::string_view cmd)
{
    tcp_.send(fmt::format("{}\r\n", cmd));
}

void SmtpClient::enter_error(std::string_view msg)
{
    if (state_ == State::Error)
        return;

    state_ = State::Error;
    active_ = false;

    // Delivered messages leave the queue: a later send_mail() — from the
    // handler too — sends the failed one and those after it, not them again.
    messages_.erase(messages_.begin(),
                    messages_.begin() + static_cast<std::ptrdiff_t>(std::min(current_msg_, messages_.size())));
    current_msg_ = 0;

    // The connection goes first (its on_close finds the client in Error and
    // reports nothing); the handler of the failed message runs last, from a
    // copy of the message: it may destroy this client, or send again.
    std::optional<SmtpMessage> failed;
    if (!messages_.empty() && messages_.front().on_error)
        failed = messages_.front();
    const std::string why(msg);

    tcp_.close();

    if (failed)
        failed->on_error(*failed, why);
}

// ─── Message serialization ───────────────────────────────────────────────────

std::string SmtpClient::build_message_data(const SmtpMessage& msg)
{
    std::string out;
    out.reserve(256 + msg.body.size());

    // If the body already contains full MIME headers (from CreateMailBody),
    // use it as-is — don't wrap with another set of headers.
    bool raw_mime = (msg.body.find("MIME-Version:") != std::string::npos
                  && msg.body.find("Content-Type:") != std::string::npos);

    if (!raw_mime) {
        out += fmt::format("From: {}\r\n", msg.from);
        for (const auto& to : msg.to)
            out += fmt::format("To: {}\r\n", to);
        out += fmt::format("Subject: {}\r\n", msg.subject);
        out += "MIME-Version: 1.0\r\n";
        out += fmt::format("Content-Type: {}; charset=UTF-8\r\n", msg.content_type);
        out += "\r\n";
    }

    // Dot-stuffing: lines starting with '.' get an extra '.'
    std::string_view body = msg.body;
    bool at_line_start = true;
    for (char c : body) {
        if (at_line_start && c == '.')
            out += '.';
        out += c;
        at_line_start = (c == '\n');
    }

    return out;
}

} // namespace apostol

#endif // WITH_SSL
