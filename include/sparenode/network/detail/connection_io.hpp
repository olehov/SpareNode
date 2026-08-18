#pragma once

#include <cstddef>
#include <span>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{

/// Abstracts the immediate native byte-transfer calls from readiness control flow.
///
/// Production uses NativeSocketOperations. Tests can supply deterministic short
/// transfers and failures without adding test hooks to TcpConnection.
class SocketOperations
{
  public:
    virtual ~SocketOperations() = default;

    SocketOperations(const SocketOperations &) = delete;
    SocketOperations &operator=(const SocketOperations &) = delete;
    SocketOperations(SocketOperations &&) = delete;
    SocketOperations &operator=(SocketOperations &&) = delete;

    [[nodiscard]] virtual std::ptrdiff_t receive(NativeSocket socket,
                                                 std::span<std::byte> buffer) noexcept = 0;
    [[nodiscard]] virtual std::ptrdiff_t send(NativeSocket socket,
                                              std::span<const std::byte> buffer) noexcept = 0;
    [[nodiscard]] virtual int last_error() const noexcept = 0;

  protected:
    SocketOperations() = default;
};

/// Performs byte transfers through the host socket API.
class NativeSocketOperations final : public SocketOperations
{
  public:
    [[nodiscard]] std::ptrdiff_t receive(NativeSocket socket,
                                         std::span<std::byte> buffer) noexcept override;
    [[nodiscard]] std::ptrdiff_t send(NativeSocket socket,
                                      std::span<const std::byte> buffer) noexcept override;
    [[nodiscard]] int last_error() const noexcept override;
};

/// Groups the stable collaborators used for I/O on one connection socket.
///
/// This non-owning context keeps dependency injection explicit without passing
/// each socket-related collaborator through every function layer.
struct ConnectionIoContext
{
    SocketWaitContext wait;
    SocketOperations &operations;
};

/// Coordinates readiness, cancellation, and native transfers for one connection.
class ConnectionIo final
{
  public:
    explicit ConnectionIo(ConnectionIoContext context) noexcept;

    /// Runs one cancellable, possibly partial receive operation.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer,
                                                            const std::stop_token &stop_token);

    /// Runs one cancellable, possibly partial send operation.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer,
                                                         const std::stop_token &stop_token);

  private:
    ConnectionIoContext context_;
};

} // namespace sparenode::network::detail
