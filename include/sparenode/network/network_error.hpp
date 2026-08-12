#pragma once

#include <cstdint>

namespace sparenode::network
{

/// Identifies the network step that failed.
enum class NetworkOperation : std::uint8_t
{
    initialize,
    resolve_address,
    create_socket,
    configure_socket,
    bind,
    listen,
    accept,
    query_local_endpoint,
    query_peer_endpoint,
};

/// Identifies which subsystem produced an error code.
enum class NetworkErrorDomain : std::uint8_t
{
    validation,
    address_resolution,
    socket,
    state,
};

/// A structured network failure that does not expose sensitive diagnostic text.
struct NetworkError
{
    /// The operation being performed when the failure occurred.
    NetworkOperation operation{};

    /// How the platform-specific numeric code should be interpreted.
    NetworkErrorDomain domain{};

    /// errno, WSAGetLastError(), getaddrinfo(), or a SpareNode-defined state code.
    int code{};

    /// Compares every structured field, which is useful in tests and error handling.
    friend constexpr bool operator==(const NetworkError &, const NetworkError &) = default;
};

} // namespace sparenode::network
