#pragma once

#include "apostol/event_loop.hpp"
#include "apostol/udp.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace apostol
{

// ─── UdpClient ──────────────────────────────────────────────────────────────
//
// Async UDP client — wraps UdpSocket + EventLoop registration.
//
// Usage:
//   UdpClient client(loop, 0);
//   client.on_datagram([](const UdpDatagram& dg) { /* ... */ });
//   client.start();
//   client.send_to("hello", "127.0.0.1", 5000);
//
class UdpClient
{
public:
    UdpClient(EventLoop& loop, uint16_t local_port = 0,
              std::string_view bind_addr = "0.0.0.0");
    ~UdpClient();

    UdpClient(const UdpClient&)            = delete;
    UdpClient& operator=(const UdpClient&) = delete;
    UdpClient(UdpClient&&)                 = delete;
    UdpClient& operator=(UdpClient&&)      = delete;

    /// Register socket with EventLoop (EPOLLIN).
    void start();

    /// Remove from EventLoop.
    void stop();

    bool running() const noexcept { return running_; }
    int fd() const noexcept { return sock_.fd(); }
    uint16_t local_port() const { return sock_.local_port(); }

    /// Send datagram to host:port (uses getaddrinfo for resolution).
    ssize_t send_to(std::string_view data, std::string_view host, uint16_t port);

    /// Send datagram to a raw sockaddr.
    ssize_t send_to(std::string_view data, const sockaddr_storage& addr, socklen_t len);

    /// Reply to the sender of a received datagram.
    ssize_t reply(const UdpDatagram& dgram, std::string_view data);

    void on_datagram(std::function<void(const UdpDatagram&)> cb) { on_datagram_ = std::move(cb); }
    void on_error(std::function<void(std::string_view)> cb)      { on_error_    = std::move(cb); }

private:
    EventLoop& loop_;
    UdpSocket  sock_;
    bool       running_{false};

    std::function<void(const UdpDatagram&)> on_datagram_;
    std::function<void(std::string_view)>   on_error_;
};

} // namespace apostol
