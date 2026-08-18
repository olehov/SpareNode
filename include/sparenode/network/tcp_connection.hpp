#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

class TcpListener;

/// Owns one accepted TCP socket and releases it automatically.
///
/// Instances are not thread-safe. Do not run operations concurrently on the
/// same connection or move or destroy it until an operation has returned.
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

    /// Waits for and receives at most one caller-provided buffer of bytes.
    ///
    /// A zero-byte success means that the peer performed an orderly shutdown.
    /// The operation may return fewer bytes than the buffer can hold.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer);

    /// Receives bytes while allowing another thread to request cancellation.
    ///
    /// Cancellation never closes the connection. If bytes are received before
    /// the stop request is observed, that successful transfer wins the race.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer,
                                                            const std::stop_token &stop_token);

    /// Waits for and sends at most one caller-provided buffer of bytes.
    ///
    /// A successful operation may send fewer bytes than supplied; callers that
    /// require full delivery must continue with the remaining suffix.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer);

    /// Sends bytes while allowing another thread to request cancellation.
    ///
    /// Cancellation never closes the connection. Bytes reported as sent remain
    /// sent even if cancellation is requested concurrently.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer,
                                                         const std::stop_token &stop_token);

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
