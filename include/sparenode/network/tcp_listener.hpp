#pragma once

#include <memory>
#include <stop_token>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

/// @brief Owns a listening TCP socket without exposing platform-specific handle types.
///
/// Instances are not thread-safe. Do not call methods concurrently on the same
/// listener or destroy it while an operation is in progress.
class TcpListener final
{
  public:
    /// @brief Closes the listening socket if this object still owns one.
    ~TcpListener();

    /// @brief Transfers socket ownership and leaves the source object closed.
    /// @param[in,out] other Listener whose socket ownership is transferred.
    TcpListener(TcpListener &&other) noexcept;

    /// @brief Releases the current socket, then takes ownership from the source object.
    /// @param[in,out] other Listener whose socket ownership is transferred.
    /// @return This listener after the ownership transfer.
    TcpListener &operator=(TcpListener &&other) noexcept;

    /// @brief Copying is forbidden because a native socket must have one owner.
    TcpListener(const TcpListener &) = delete;
    /// @brief Copy assignment is forbidden because a native socket must have one owner.
    TcpListener &operator=(const TcpListener &) = delete;

    /// @brief Binds a numeric IPv4 or IPv6 address and starts listening.
    ///
    /// Port zero asks the operating system to select an available port. Wildcard
    /// addresses must be supplied explicitly; an empty address and host names are
    /// rejected. The backlog controls the operating system's pending connection queue.
    /// @param[in] endpoint Numeric local address and TCP port to bind.
    /// @param[in] backlog Maximum pending-connection queue requested from the system.
    /// @return An owning listener, or a structured validation, resolution, or socket error.
    [[nodiscard]] static Result<TcpListener, NetworkError> bind(const TcpEndpoint &endpoint,
                                                                int backlog = 128);

    /// @brief Blocks until a connection is accepted or the operating system
    /// reports an error.
    /// @return An owning connection, or a structured network error.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept();

    /// @brief Waits for a connection while allowing another thread to request cancellation.
    ///
    /// Cancellation wakes an already-blocked wait and returns an `accept` error in
    /// the `cancellation` domain. The listener remains open and can accept again.
    /// Requesting stop before this call begins cancels it immediately. Do not run
    /// more than one accept operation or other listener methods concurrently.
    /// @param[in] stop_token Token observed while waiting for listener readiness.
    /// @return An owning connection, or a structured network or cancellation error.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept(const std::stop_token &stop_token);

    /// @brief Returns the bound address and effective port selected by the system.
    /// @return The local endpoint; a state error when the listener is closed; or a
    /// structured socket error when the native endpoint query fails.
    [[nodiscard]] Result<TcpEndpoint, NetworkError> local_endpoint() const;

    /// @brief Reports whether this object owns an open native socket.
    /// @return `true` while the listener owns a valid socket.
    [[nodiscard]] bool is_open() const noexcept;

  private:
    /// @brief Platform-specific implementation hidden from the public API.
    struct Impl;

    /// @brief Creates a public listener around an implementation that owns a socket.
    /// @param[in] impl Implementation whose ownership is transferred to this object.
    explicit TcpListener(std::unique_ptr<Impl> impl) noexcept;

    /// @brief Attempts one nonblocking native accept after listener readiness.
    /// @return An owning connection, or a structured socket error.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept_ready_connection();

    /// @brief Owned platform-specific listener state, or null after a move.
    std::unique_ptr<Impl> impl_;
};

} // namespace sparenode::network
