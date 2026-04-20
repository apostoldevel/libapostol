#include "apostol/udp_client.hpp"

#include <netdb.h>
#include <cstring>

#include <fmt/format.h>

namespace apostol
{

UdpClient::UdpClient(EventLoop& loop, uint16_t local_port, std::string_view bind_addr)
    : loop_(loop)
    , sock_(local_port, bind_addr)
{}

UdpClient::~UdpClient()
{
    if (running_)
        stop();
}

void UdpClient::start()
{
    if (running_)
        return;

    loop_.add_io(sock_.fd(), EPOLLIN, [this](uint32_t) {
        for (;;) {
            auto dgram = sock_.recv();
            if (!dgram)
                break;  // EAGAIN — drained
            if (on_datagram_)
                on_datagram_(*dgram);
        }
        // Under APOSTOL_EPOLL_ET the fd was armed with EPOLLONESHOT and
        // disabled after this event. Rearm for the next datagram batch.
        // Under LT flag OFF rearm_io is a no-op.
        if (running_)
            loop_.rearm_io(sock_.fd(), EPOLLIN);
    });
    running_ = true;
}

void UdpClient::stop()
{
    if (!running_)
        return;

    loop_.remove_io(sock_.fd());
    running_ = false;
}

ssize_t UdpClient::send_to(std::string_view data, std::string_view host, uint16_t port)
{
    std::string host_str(host);

    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICSERV;

    auto port_str = fmt::format("{}", port);

    struct addrinfo* result = nullptr;
    int rc = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        if (on_error_)
            on_error_(fmt::format("getaddrinfo({}): {}", host, gai_strerror(rc)));
        return -1;
    }

    sockaddr_storage addr{};
    std::memcpy(&addr, result->ai_addr, result->ai_addrlen);
    socklen_t addr_len = result->ai_addrlen;
    ::freeaddrinfo(result);

    return sock_.send(data.data(), data.size(), addr, addr_len);
}

ssize_t UdpClient::send_to(std::string_view data, const sockaddr_storage& addr, socklen_t len)
{
    return sock_.send(data.data(), data.size(), addr, len);
}

ssize_t UdpClient::reply(const UdpDatagram& dgram, std::string_view data)
{
    return sock_.reply(dgram, data);
}

} // namespace apostol
