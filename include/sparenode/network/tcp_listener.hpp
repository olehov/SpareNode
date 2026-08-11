#pragma once

#include <memory>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

/// Owns a listening TCP socket without exposing platform-specific handle types.
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

    /// Blocks until a connection is accepted or the operating system reports an error.
    [[nodiscard]] Result<TcpConnection, NetworkError> accept();

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
