#pragma once

#include <cstdint>
#include <string>

namespace sparenode::network
{

/// @brief A numeric IP address and TCP port.
///
/// Listener APIs deliberately do not resolve host names. Port zero asks the
/// operating system to select an available ephemeral port.
struct TcpEndpoint
{
    /// @brief Numeric IPv4 or IPv6 text, for example "127.0.0.1" or "::1".
    std::string address;

    /// @brief TCP port in host byte order.
    std::uint16_t port{};

    /// @brief Compares both the numeric address and port.
    /// @param[in] lhs Left-hand endpoint.
    /// @param[in] rhs Right-hand endpoint.
    /// @return `true` when both address and port are equal.
    friend bool operator==(const TcpEndpoint &lhs, const TcpEndpoint &rhs) = default;
};

} // namespace sparenode::network
