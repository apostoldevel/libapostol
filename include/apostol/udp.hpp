#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>

namespace apostol
{

/// Result of recvfrom: data + sender address.
struct UdpDatagram
{
    std::string      data;
    sockaddr_storage peer_addr{};
    socklen_t        peer_len{0};

    std::string peer_ip() const;
    uint16_t    peer_port() const;
};

/// RAII wrapper for a non-blocking UDP socket.
class UdpSocket
{
public:
    explicit UdpSocket(uint16_t port, std::string_view bind_addr = "0.0.0.0");
    ~UdpSocket();

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;

    int      fd()         const noexcept { return fd_; }
    uint16_t local_port() const;

    std::optional<UdpDatagram> recv();
    ssize_t send(const void* data, std::size_t len,
                 const sockaddr_storage& addr, socklen_t addr_len);
    ssize_t reply(const UdpDatagram& dgram, std::string_view data);

private:
    int fd_{-1};
};

} // namespace apostol
