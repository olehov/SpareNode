#pragma once

#include <cstdint>
#include <memory>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"

namespace sparenode::network::detail
{

/// @brief Identifies which readiness condition a socket operation requires.
enum class SocketWaitInterest : std::uint8_t
{
    readable, ///< Wait until bytes or a connection can be read.
    writable, ///< Wait until bytes can be written.
};

/// @brief Describes why a socket-readiness wait completed.
enum class SocketWaitStatus : std::uint8_t
{
    socket_ready,  ///< The requested readiness condition was reported.
    socket_error,  ///< The poller reported a socket error.
    socket_hangup, ///< The poller reported a peer hangup.
    cancelled,     ///< The stop token requested cancellation.
};

/// @brief Describes the readiness event and public operation represented by one wait.
struct SocketWaitRequest
{
    /// @brief Read or write readiness requested from the poller.
    SocketWaitInterest interest;
    /// @brief Public operation to attach to a wait failure.
    NetworkOperation operation;
};

/// @brief Lazily owns the private sockets used to wake cancellable readiness waits.
///
/// One instance belongs to one listener or connection and can be reused by its
/// sequential operations. The owning socket abstraction remains non-thread-safe.
class SocketWakeChannel final
{
  public:
    /// @brief Constructs an uninitialized wake channel without allocating sockets.
    SocketWakeChannel() noexcept;
    /// @brief Closes the private wake sockets if they were initialized.
    ~SocketWakeChannel();

    /// @brief Copying is forbidden because the wake sockets have one owner.
    SocketWakeChannel(const SocketWakeChannel &) = delete;
    /// @brief Copy assignment is forbidden because the wake sockets have one owner.
    SocketWakeChannel &operator=(const SocketWakeChannel &) = delete;
    /// @brief Moving is forbidden because wait contexts retain a stable reference.
    SocketWakeChannel(SocketWakeChannel &&) = delete;
    /// @brief Move assignment is forbidden because wait contexts retain a stable reference.
    SocketWakeChannel &operator=(SocketWakeChannel &&) = delete;

    /// @brief Creates and connects the channel on first use; later calls reuse it.
    /// @param[in] operation Public operation to record if initialization fails.
    /// @return Success, or a structured socket error.
    [[nodiscard]] Result<void, NetworkError> ensure_initialized(NetworkOperation operation);

    /// @brief Sends one wake byte if the channel has been initialized.
    void notify() const noexcept;

    /// @brief Drains all pending wake bytes without blocking.
    void drain() const noexcept;

    /// @brief Returns the reader watched by the poller.
    /// @return Borrowed reader socket, or invalid_socket before initialization.
    [[nodiscard]] NativeSocket reader() const noexcept;

    /// @brief Reports whether native wake resources have already been created.
    /// @return `true` after successful lazy initialization.
    [[nodiscard]] bool is_initialized() const noexcept;

  private:
    /// @brief Platform-specific owned wake-channel state.
    struct Impl;
    /// @brief Creates and connects the platform wake-channel sockets.
    /// @param[in] operation Public operation to record if creation fails.
    /// @return Owned implementation, or a structured socket error.
    [[nodiscard]] static Result<std::unique_ptr<Impl>, NetworkError>
    create(NetworkOperation operation);

    /// @brief Owned wake-channel state, or null before lazy initialization.
    std::unique_ptr<Impl> impl_;
};

/// @brief Groups stable resources used by readiness waits on one native socket.
///
/// The context does not own its socket or collaborators. They must outlive each
/// wait performed through this context.
struct SocketWaitContext
{
    /// @brief Borrowed operation socket watched for readiness.
    NativeSocket socket;
    /// @brief Poller used for the blocking native wait.
    SocketPoller &poller;
    /// @brief Reusable wake channel used only by cancellable waits.
    SocketWakeChannel &wake_channel;
};

/// @brief Waits indefinitely for one socket without allocating cancellation resources.
/// @param[in] context Borrowed stable socket and wait collaborators.
/// @param[in] request Readiness interest and public operation metadata.
/// @return Completion status, or a structured polling error.
[[nodiscard]] Result<SocketWaitStatus, NetworkError> wait_for_socket(SocketWaitContext context,
                                                                     SocketWaitRequest request);

/// @brief Waits for one socket while allowing a stop request to wake the native poll.
///
/// The supplied loopback wake channel is initialized lazily only when the token
/// can be stopped, then reused by later waits. The operation socket itself is
/// never closed to interrupt the wait.
///
/// @param[in] context Borrowed stable socket and wait collaborators.
/// @param[in] request Readiness interest and public operation metadata.
/// @param[in] stop_token Token observed before and during the blocking wait.
/// @return Completion status, or a structured polling or initialization error.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
wait_for_socket(SocketWaitContext context, SocketWaitRequest request,
                const std::stop_token &stop_token);

} // namespace sparenode::network::detail
