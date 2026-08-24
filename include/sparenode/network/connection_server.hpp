#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "sparenode/network/connection_dispatcher.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

/// @brief Identifies which resource prevented a connection server from starting.
enum class ConnectionServerStartErrorCode : std::uint8_t
{
    listener_start_failed,      ///< The listening endpoint could not be opened.
    dispatcher_start_failed,    ///< The bounded worker pool could not be created.
    accept_thread_start_failed, ///< The dedicated accept thread could not be created.
    resource_allocation_failed, ///< Server-owned state could not be allocated.
};

/// @brief Describes a structured connection-server startup failure.
struct ConnectionServerStartError
{
    /// @brief Portable startup failure category.
    ConnectionServerStartErrorCode code{};

    /// @brief Listener failure, present only for `listener_start_failed`.
    std::optional<NetworkError> network_error;

    /// @brief Dispatcher failure, present only for `dispatcher_start_failed`.
    std::optional<DispatchError> dispatch_error;

    /// @brief Native thread-start error code, or zero when unavailable.
    int native_code{};
};

/// @brief Identifies which accept-loop boundary isolated a runtime failure.
enum class ConnectionServerFailureKind : std::uint8_t
{
    accept_error,       ///< The listener could no longer accept connections.
    dispatch_error,     ///< An accepted connection could not enter the dispatcher.
    internal_exception, ///< An unexpected exception crossed the accept-loop boundary.
};

/// @brief Reports an accept-loop failure without transferring server ownership.
struct ConnectionServerFailure
{
    /// @brief Portable runtime failure category.
    ConnectionServerFailureKind kind{};

    /// @brief Listener error, present only for `accept_error`.
    std::optional<NetworkError> network_error;

    /// @brief Dispatcher error, present only for `dispatch_error`.
    std::optional<DispatchError> dispatch_error;
};

/// @brief Observes accept-loop failures that are isolated from connection handlers.
///
/// The observer runs on the accept thread. Exceptions are contained, but the
/// callback must not destroy or move the server that invoked it.
using ConnectionServerFailureObserver = std::function<void(const ConnectionServerFailure &failure)>;

/// @brief Groups listener, dispatcher, and observer settings for one server.
struct ConnectionServerConfig
{
    /// @brief Numeric local endpoint used to open the listening socket.
    TcpEndpoint endpoint;

    /// @brief Maximum pending-connection queue requested from the operating system.
    int listen_backlog{128};

    /// @brief Enables the configured worker count; `false` forces one worker.
    bool multithreading_enabled{false};

    /// @brief Fixed worker-pool limits and per-connection callbacks.
    ConnectionDispatcherConfig dispatcher;

    /// @brief Optional observer for failures at the accept or dispatch boundary.
    ConnectionServerFailureObserver failure_observer;

    /// @brief Resolves the worker count allowed by the multithreading switch.
    /// @return Configured worker count when enabled, otherwise exactly one worker.
    [[nodiscard]] constexpr std::size_t effective_worker_count() const noexcept
    {
        return multithreading_enabled ? dispatcher.options.worker_count : 1;
    }
};

/// @brief Accepts TCP clients and dispatches them through a bounded fixed worker pool.
///
/// The server owns one long-lived accept thread and the fixed worker count configured
/// in `ConnectionDispatcherConfig`; it never creates one thread per connection.
/// Destruction requests cancellation and waits for the accept thread and all workers.
///
/// @warning Destruction and moves must not overlap any operation on the same server.
/// They must not run from the server's handler, connection-failure, or lifecycle-
/// failure callbacks because a managed thread cannot join itself.
class ConnectionServer final
{
  public:
    /// @brief Opens a listener, creates the dispatcher, and starts accepting clients.
    /// @param[in] config Endpoint, resource limits, handlers, and observers.
    /// @return A running server, or a structured startup failure.
    [[nodiscard]] static Result<ConnectionServer, ConnectionServerStartError>
    start(ConnectionServerConfig config);

    /// @brief Requests shutdown and joins the accept thread and dispatcher workers.
    ~ConnectionServer();

    /// @brief Transfers ownership of a running server without relocating its threads.
    /// @param[in,out] other Server whose implementation is transferred.
    ConnectionServer(ConnectionServer &&other) noexcept;

    /// @brief Stops the current server before taking ownership from another.
    /// @param[in,out] other Server whose implementation is transferred.
    /// @return This server after the ownership transfer.
    ConnectionServer &operator=(ConnectionServer &&other) noexcept;

    /// @brief Copying is forbidden because server resources have one owner.
    ConnectionServer(const ConnectionServer &) = delete;
    /// @brief Copy assignment is forbidden because server resources have one owner.
    ConnectionServer &operator=(const ConnectionServer &) = delete;

    /// @brief Returns the numeric endpoint selected before the accept loop started.
    /// @return Bound endpoint, or no value for a moved-from server.
    [[nodiscard]] std::optional<TcpEndpoint> local_endpoint() const;

    /// @brief Requests cooperative cancellation without waiting for threads to join.
    ///
    /// The call is idempotent. It wakes a blocked accept, cancels a blocked dispatcher
    /// submission, closes queued connections, and requests active handler stop tokens.
    void request_stop() noexcept;

  private:
    /// @brief Stable implementation shared by the accept loop and public owner.
    struct Impl;

    /// @brief Wraps a fully started server implementation.
    /// @param[in] impl Owned implementation transferred to the public server.
    explicit ConnectionServer(std::unique_ptr<Impl> impl) noexcept;

    /// @brief Owned server state, or null after a move.
    std::unique_ptr<Impl> impl_;
};

} // namespace sparenode::network
