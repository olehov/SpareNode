#pragma once

#include <span>

#include "sparenode/network/detail/native_socket.hpp"

namespace sparenode::network::detail
{

/// @brief Describes one socket and the readiness events requested by a poll operation.
struct SocketPollEntry
{
    /// @brief Borrowed native socket watched by the poller.
    NativeSocket socket{invalid_socket};
    /// @brief Whether read readiness should be requested.
    bool watch_readable{false};
    /// @brief Whether write readiness should be requested.
    bool watch_writable{false};
    /// @brief Whether the completed poll reported read readiness.
    bool readable{false};
    /// @brief Whether the completed poll reported write readiness.
    bool writable{false};
    /// @brief Whether the completed poll reported a socket error.
    bool error{false};
    /// @brief Whether the completed poll reported a peer hangup.
    bool hangup{false};
    /// @brief Whether the completed poll reported an invalid descriptor.
    bool invalid{false};
};

/// @brief Waits for readiness without exposing a platform-specific polling API.
///
/// Higher-level networking code depends on this interface, while platform code
/// translates entries to `WSAPoll` on Windows and `poll` on POSIX systems.
class SocketPoller
{
  public:
    /// @brief Enables polymorphic destruction of poller implementations.
    virtual ~SocketPoller() = default;

    /// @brief Pollers are non-copyable because implementations may own native state.
    SocketPoller(const SocketPoller &) = delete;
    /// @brief Pollers are non-copy-assignable.
    SocketPoller &operator=(const SocketPoller &) = delete;
    /// @brief Pollers are non-movable because users retain stable references to them.
    SocketPoller(SocketPoller &&) = delete;
    /// @brief Pollers are non-move-assignable.
    SocketPoller &operator=(SocketPoller &&) = delete;

    /// @brief Blocks until at least one requested event occurs or polling fails.
    /// @param[in,out] entries Requested events on input and reported events on output.
    /// @param[in] operation Public operation to record if polling fails.
    /// @return Success after readiness, or a structured socket error.
    [[nodiscard]] virtual Result<void, NetworkError> wait(std::span<SocketPollEntry> entries,
                                                          NetworkOperation operation) = 0;

  protected:
    /// @brief Constructs the interface for a concrete poller implementation.
    SocketPoller() = default;
};

/// @brief Implements the cross-platform readiness wait using the host socket API.
class NativeSocketPoller final : public SocketPoller
{
  public:
    /// @brief Constructs a stateless native poller.
    NativeSocketPoller() = default;

    /// @brief Translates portable entries to native descriptors and waits indefinitely.
    /// @param[in,out] entries Requested events on input and reported events on output.
    /// @param[in] operation Public operation to record if polling fails.
    /// @return Success after readiness, or a structured socket error.
    [[nodiscard]] Result<void, NetworkError> wait(std::span<SocketPollEntry> entries,
                                                  NetworkOperation operation) override;
};

} // namespace sparenode::network::detail
