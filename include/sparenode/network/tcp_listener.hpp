#pragma once

#include <memory>
#include <stop_token>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

/// Owns a listening TCP socket without exposing platform-specific handle types.
///
/// Instances are not thread-safe. Do not call methods concurrently on the same
/// listener or destroy it while an operation is in progress.
class TcpListener final
{
  public:
    /// Closes the listening socket if this object still owns one.
    ~TcpListener();

    /// Transfers socket ownership and leaves the source object closed.
    TcpListener(TcpListener &&) noexcept;

    /// Releases the current socket, then takes ownership from the source object.
    TcpListener &operator=(TcpListener &&) noexcept;

    // A native socket must have exactly one owner, so copying is forbidden.
    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;

    /// Binds a numeric IPv4 or IPv6 address and starts listening.
    ///
    /// Port zero asks the operating system to select an available port. Wildcard
    /// addresses must be supplied explicitly; an empty address and host names are
    /// rejected. The backlog controls the operating system's pending connection queue.
    [[nodiscard]] static Result<TcpListener, NetworkError> bind(const TcpEndpoint &endpoint,
                                                                int backlog = 128);

    /// Blocks indefinitely until a connection is accepted or the operating system
    /// reports an error.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept();

    /// Waits for a connection while allowing another thread to request cancellation.
    ///
    /// Cancellation wakes an already-blocked wait and returns an `accept` error in
    /// the `cancellation` domain. The listener remains open and can accept again.
    /// Requesting stop before this call begins cancels it immediately. Do not run
    /// more than one accept operation or other listener methods concurrently.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept(const std::stop_token &stop_token);

    /// Returns the bound address and the effective port selected by the system.
    [[nodiscard]] Result<TcpEndpoint, NetworkError> local_endpoint() const;

    /// Returns true while this object owns an open native socket.
    [[nodiscard]] bool is_open() const noexcept;

  private:
    // PImpl keeps Windows and Linux socket definitions out of the public API.
    struct Impl;

    /// Creates a public listener around an implementation that already owns a socket.
    explicit TcpListener(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace sparenode::network
