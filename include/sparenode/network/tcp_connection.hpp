#pragma once

#include <memory>
#include <optional>

#include "sparenode/network/tcp_endpoint.hpp"

namespace sparenode::network
{

class TcpListener;

/// Owns one accepted TCP socket and releases it automatically.
class TcpConnection final
{
  public:
    /// Closes the accepted socket if this object still owns one.
    ~TcpConnection();

    /// Transfers socket ownership and leaves the source object closed.
    TcpConnection(TcpConnection &&) noexcept;

    /// Releases the current socket, then takes ownership from the source object.
    TcpConnection &operator=(TcpConnection &&) noexcept;

    // A native socket must have exactly one owner, so copying is forbidden.
    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    /// Returns true while this object owns an open native socket.
    [[nodiscard]] bool is_open() const noexcept;

    /// Returns the remote endpoint, or no value for a moved-from connection.
    [[nodiscard]] std::optional<TcpEndpoint> peer_endpoint() const;

  private:
    // PImpl keeps SOCKET/file-descriptor types out of this public header.
    struct Impl;

    /// Creates a public connection around an implementation that already owns a socket.
    explicit TcpConnection(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    // Only a listener can create a connection from a freshly accepted socket.
    friend class TcpListener;
};

} // namespace sparenode::network
