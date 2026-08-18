#pragma once

#include <cstdint>
#include <memory>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"

namespace sparenode::network::detail
{

/// Identifies which readiness condition a socket operation requires.
enum class SocketWaitInterest : std::uint8_t
{
    readable,
    writable,
};

/// Describes why a socket-readiness wait completed.
enum class SocketWaitStatus : std::uint8_t
{
    socket_ready,
    socket_error,
    socket_hangup,
    cancelled,
};

/// Describes the readiness event and public operation represented by one wait.
struct SocketWaitRequest
{
    SocketWaitInterest interest;
    NetworkOperation operation;
};

/// Lazily owns the private sockets used to wake cancellable readiness waits.
///
/// One instance belongs to one listener or connection and can be reused by its
/// sequential operations. The owning socket abstraction remains non-thread-safe.
class SocketWakeChannel final
{
  public:
    SocketWakeChannel() noexcept;
    ~SocketWakeChannel();

    SocketWakeChannel(const SocketWakeChannel &) = delete;
    SocketWakeChannel &operator=(const SocketWakeChannel &) = delete;
    SocketWakeChannel(SocketWakeChannel &&) = delete;
    SocketWakeChannel &operator=(SocketWakeChannel &&) = delete;

    /// Creates and connects the channel on first use; later calls reuse it.
    [[nodiscard]] Result<void, NetworkError> ensure_initialized(NetworkOperation operation);

    /// Sends one wake byte if the channel has been initialized.
    void notify() const noexcept;

    /// Drains all pending wake bytes without blocking.
    void drain() const noexcept;

    /// Returns the reader watched by the poller, or invalid_socket before initialization.
    [[nodiscard]] NativeSocket reader() const noexcept;

    /// Reports whether native wake resources have already been created.
    [[nodiscard]] bool is_initialized() const noexcept;

  private:
    struct Impl;
    [[nodiscard]] static Result<std::unique_ptr<Impl>, NetworkError>
    create(NetworkOperation operation);

    std::unique_ptr<Impl> impl_;
};

/// Groups the stable resources used by readiness waits on one native socket.
///
/// The context does not own its socket or collaborators. They must outlive each
/// wait performed through this context.
struct SocketWaitContext
{
    NativeSocket socket;
    SocketPoller &poller;
    SocketWakeChannel &wake_channel;
};

/// Waits indefinitely for one socket without allocating cancellation resources.
[[nodiscard]] Result<SocketWaitStatus, NetworkError> wait_for_socket(SocketWaitContext context,
                                                                     SocketWaitRequest request);

/// Waits for one socket while allowing a stop request to wake the native poll.
///
/// The supplied loopback wake channel is initialized lazily only when the token
/// can be stopped, then reused by later waits. The operation socket itself is
/// never closed to interrupt the wait.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
wait_for_socket(SocketWaitContext context, SocketWaitRequest request,
                const std::stop_token &stop_token);

} // namespace sparenode::network::detail
