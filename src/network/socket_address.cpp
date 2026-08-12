#include "sparenode/network/detail/socket_address.hpp"

#include <array>
#include <cstdint>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace sparenode::network::detail
{

/// Adopts the linked list returned by getaddrinfo.
AddressInfo::AddressInfo(addrinfo *value) noexcept : value_(value)
{
}

/// Returns the entire address list to the platform resolver.
AddressInfo::~AddressInfo()
{
    if (value_ != nullptr)
    {
        freeaddrinfo(value_);
    }
}

/// Exposes the first candidate for read-only traversal by the binding code.
addrinfo *AddressInfo::get() const noexcept
{
    return value_;
}

/// Translates a native IPv4 or IPv6 socket address into the public endpoint type.
Result<TcpEndpoint, NetworkError> endpoint_from_address(const sockaddr *address,
                                                        const NetworkOperation operation)
{
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const void *raw_address = nullptr;
    std::uint16_t port = 0;

    if (address->sa_family == AF_INET)
    {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
        raw_address = &ipv4->sin_addr;
        port = ntohs(ipv4->sin_port);
    }
    else if (address->sa_family == AF_INET6)
    {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(address);
        raw_address = &ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
    }
    else
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::state, 1});
    }

    if (inet_ntop(address->sa_family, raw_address, buffer.data(), buffer.size()) == nullptr)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    return TcpEndpoint{buffer.data(), port};
}

} // namespace sparenode::network::detail
