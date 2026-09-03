#pragma once

#include <cstdint>

namespace sparenode::network
{

/// @brief Identifies the network step that failed.
enum class NetworkOperation : std::uint8_t
{
    initialize,           ///< Initialize process-wide networking facilities.
    resolve_address,      ///< Translate a numeric endpoint to a native address.
    create_socket,        ///< Create a native socket.
    configure_socket,     ///< Apply required native socket options.
    bind,                 ///< Bind a socket to a local address.
    listen,               ///< Start accepting connection requests.
    accept,               ///< Accept an incoming connection.
    receive,              ///< Receive bytes from a connection.
    send,                 ///< Send bytes through a connection.
    query_local_endpoint, ///< Query the address bound to a socket.
    query_peer_endpoint,  ///< Query the address of a connected peer.
};

/// @brief Identifies which subsystem produced an error code.
enum class NetworkErrorDomain : std::uint8_t
{
    validation,         ///< A public argument violated an API contract.
    address_resolution, ///< The platform address resolver rejected an endpoint.
    socket,             ///< The host socket API reported an error.
    state,              ///< The object is not open or is otherwise unusable.
    cancellation,       ///< A caller-provided stop token requested cancellation.
    timeout,            ///< An absolute monotonic I/O deadline expired.
};

/// @brief A structured network failure without sensitive diagnostic text.
struct NetworkError
{
    /// @brief Operation being performed when the failure occurred.
    NetworkOperation operation{};

    /// @brief Domain used to interpret the numeric error code.
    NetworkErrorDomain domain{};

    /// @brief Native or SpareNode-defined numeric error code.
    ///
    /// Depending on domain, this is errno, WSAGetLastError(), getaddrinfo(), or
    /// a SpareNode-defined validation, state, cancellation, or timeout code.
    int code{};

    /// @brief Compares every structured error field.
    /// @param[in] lhs Left-hand error.
    /// @param[in] rhs Right-hand error.
    /// @return `true` when operation, domain, and code are equal.
    friend constexpr bool operator==(const NetworkError &lhs, const NetworkError &rhs) = default;
};

} // namespace sparenode::network
