#pragma once

#include <cstddef>
#include <span>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{

/// @brief Abstracts native byte-transfer calls from readiness control flow.
///
/// Production uses NativeSocketOperations. Tests can supply deterministic short
/// transfers and failures without adding test hooks to TcpConnection.
class SocketOperations
{
  public:
    /// @brief Enables polymorphic destruction of socket-operation implementations.
    virtual ~SocketOperations() = default;

    /// @brief Socket-operation providers are non-copyable.
    SocketOperations(const SocketOperations &) = delete;
    /// @brief Socket-operation providers are non-copy-assignable.
    SocketOperations &operator=(const SocketOperations &) = delete;
    /// @brief Socket-operation providers are non-movable to preserve stable references.
    SocketOperations(SocketOperations &&) = delete;
    /// @brief Socket-operation providers are non-move-assignable.
    SocketOperations &operator=(SocketOperations &&) = delete;

    /// @brief Receives at most one buffer from a nonblocking socket.
    /// @param[in] socket Borrowed connected socket.
    /// @param[out] buffer Destination storage for received bytes.
    /// @return Transferred byte count, or the native failure sentinel.
    [[nodiscard]] virtual std::ptrdiff_t receive(NativeSocket socket,
                                                 std::span<std::byte> buffer) noexcept = 0;
    /// @brief Sends at most one buffer through a nonblocking socket.
    /// @param[in] socket Borrowed connected socket.
    /// @param[in] buffer Bytes available for transmission.
    /// @return Transferred byte count, or the native failure sentinel.
    [[nodiscard]] virtual std::ptrdiff_t send(NativeSocket socket,
                                              std::span<const std::byte> buffer) noexcept = 0;
    /// @brief Returns the most recent platform socket error.
    /// @return Native error code associated with the preceding failed operation.
    [[nodiscard]] virtual int last_error() const noexcept = 0;

  protected:
    /// @brief Constructs the interface for a concrete operation provider.
    SocketOperations() = default;
};

/// @brief Performs byte transfers through the host socket API.
class NativeSocketOperations final : public SocketOperations
{
  public:
    /// @brief Receives at most one buffer from a nonblocking socket.
    /// @param[in] socket Borrowed connected socket.
    /// @param[out] buffer Destination storage for received bytes.
    /// @return Transferred byte count, or the native failure sentinel.
    [[nodiscard]] std::ptrdiff_t receive(NativeSocket socket,
                                         std::span<std::byte> buffer) noexcept override;
    /// @brief Sends at most one buffer through a nonblocking socket.
    /// @param[in] socket Borrowed connected socket.
    /// @param[in] buffer Bytes available for transmission.
    /// @return Transferred byte count, or the native failure sentinel.
    [[nodiscard]] std::ptrdiff_t send(NativeSocket socket,
                                      std::span<const std::byte> buffer) noexcept override;
    /// @brief Returns the most recent platform socket error.
    /// @return Native error code associated with the preceding failed operation.
    [[nodiscard]] int last_error() const noexcept override;
};

/// @brief Groups the stable collaborators used for I/O on one connection socket.
///
/// This non-owning context keeps dependency injection explicit without passing
/// each socket-related collaborator through every function layer.
struct ConnectionIoContext
{
    /// @brief Socket readiness context used before each transfer attempt.
    // SocketWaitContext has reference members, so omitting it during aggregate
    // initialization is rejected by the compiler rather than leaving it uninitialized.
    // cppcheck-suppress uninitMemberVarNoCtor
    SocketWaitContext wait;
    /// @brief Native transfer provider used after the socket becomes ready.
    SocketOperations &operations;
};

/// @brief Coordinates readiness, cancellation, and native transfers for one connection.
class ConnectionIo final
{
  public:
    /// @brief Stores non-owning collaborators for one connection.
    /// @param[in] context Borrowed socket, poller, wake channel, and operations provider.
    explicit ConnectionIo(const ConnectionIoContext &context) noexcept;

    /// @brief Runs one cancellable, possibly partial receive operation.
    /// @param[out] buffer Destination storage for received bytes.
    /// @param[in] stop_token Token observed while waiting for read readiness.
    /// @return Transferred byte count, or a structured receive or cancellation error.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer,
                                                            const std::stop_token &stop_token);

    /// @brief Runs one cancellable, possibly partial send operation.
    /// @param[in] buffer Bytes available for transmission.
    /// @param[in] stop_token Token observed while waiting for write readiness.
    /// @return Transferred byte count, or a structured send or cancellation error.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer,
                                                         const std::stop_token &stop_token);

  private:
    /// @brief Non-owning dependencies that must outlive this coordinator.
    ConnectionIoContext context_;
};

} // namespace sparenode::network::detail
