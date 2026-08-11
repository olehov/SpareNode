#pragma once

#include <cstdint>
#include <string>

namespace sparenode::network
{

/// A numeric IP address and TCP port.
///
/// Listener APIs deliberately do not resolve host names. Port zero asks the
/// operating system to select an available ephemeral port.
struct TcpEndpoint
{
    /// Numeric IPv4 or IPv6 text, for example "127.0.0.1" or "::1".
    std::string address;

    /// TCP port in host byte order.
    std::uint16_t port{};

    /// Compares both the numeric address and port.
    friend bool operator==(const TcpEndpoint &, const TcpEndpoint &) = default;
};

} // namespace sparenode::network
