#pragma once

#include <span>

#include "sparenode/network/detail/native_socket.hpp"

namespace sparenode::network::detail
{

/// Describes one socket and the readiness events requested by a poll operation.
struct SocketPollEntry
{
    NativeSocket socket{invalid_socket};
    bool watch_readable{false};
    bool watch_writable{false};
    bool readable{false};
    bool writable{false};
    bool error{false};
    bool hangup{false};
    bool invalid{false};
};

/// Waits for readiness without exposing a platform-specific polling API.
///
/// Higher-level networking code depends on this interface, while platform code
/// translates entries to `WSAPoll` on Windows and `poll` on POSIX systems.
class SocketPoller
{
  public:
    virtual ~SocketPoller() = default;

    SocketPoller(const SocketPoller &) = delete;
    SocketPoller &operator=(const SocketPoller &) = delete;
    SocketPoller(SocketPoller &&) = delete;
    SocketPoller &operator=(SocketPoller &&) = delete;

    /// Blocks until at least one requested event occurs or polling fails.
    [[nodiscard]] virtual Result<void, NetworkError> wait(std::span<SocketPollEntry> entries,
                                                          NetworkOperation operation) = 0;

  protected:
    SocketPoller() = default;
};

/// Implements the cross-platform readiness wait using the host socket API.
class NativeSocketPoller final : public SocketPoller
{
  public:
    NativeSocketPoller() = default;

    /// Translates portable entries to native descriptors and waits indefinitely.
    [[nodiscard]] Result<void, NetworkError> wait(std::span<SocketPollEntry> entries,
                                                  NetworkOperation operation) override;
};

} // namespace sparenode::network::detail
