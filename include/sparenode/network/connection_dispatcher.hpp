#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

/// @brief Identifies why a dispatcher operation could not be completed.
enum class DispatchErrorCode : std::uint8_t
{
    invalid_worker_count,             ///< At least one worker is required.
    invalid_pending_connection_limit, ///< The pending queue must have positive capacity.
    missing_connection_handler,       ///< No callable connection handler was supplied.
    invalid_connection,               ///< The submitted connection does not own a socket.
    stopped,                          ///< The dispatcher has begun shutting down.
    cancelled,                        ///< The caller cancelled a blocked submission.
    worker_start_failed,              ///< The operating system could not create a worker.
    resource_allocation_failed,       ///< Dispatcher storage could not be allocated.
};

/// @brief Describes a dispatcher configuration or submission failure.
struct DispatchError
{
    /// @brief Portable category of the failure.
    DispatchErrorCode code{};

    /// @brief Native error code for worker startup failure, or zero otherwise.
    int native_code{};

    /// @brief Compares every structured error field.
    /// @param[in] lhs Left-hand error.
    /// @param[in] rhs Right-hand error.
    /// @return `true` when both errors describe the same failure.
    friend constexpr bool operator==(const DispatchError &lhs, const DispatchError &rhs) = default;
};

/// @brief Identifies how an asynchronous connection handler failed.
enum class ConnectionFailureKind : std::uint8_t
{
    handler_error,     ///< The handler returned a structured network error.
    handler_exception, ///< The handler unexpectedly threw an exception.
};

/// @brief Reports a failure isolated by a dispatcher worker.
struct ConnectionFailure
{
    /// @brief Portable failure category.
    ConnectionFailureKind kind{};

    /// @brief Handler-provided network error, absent for an unexpected exception.
    std::optional<NetworkError> network_error;
};

/// @brief Processes one exclusively owned connection on a dispatcher worker.
///
/// A dispatcher may invoke the same callable concurrently on different workers,
/// so captured shared state must be synchronized by the handler implementation.
/// The callable receives a connection transferred from the pending queue and a
/// stop token requested during dispatcher shutdown. It returns success after
/// processing or a structured network failure.
using ConnectionHandler = std::function<Result<void, NetworkError>(
    TcpConnection connection, const std::stop_token &stop_token)>;

/// @brief Observes an isolated handler failure without controlling worker lifetime.
///
/// Multiple workers may invoke the observer concurrently. Observer exceptions are
/// contained by the dispatcher, but the callback should remain short and non-blocking.
/// The callback receives the failure reported by a handler or worker boundary.
using ConnectionFailureObserver = std::function<void(const ConnectionFailure &failure)>;

/// @brief Defines fixed resource limits for a connection dispatcher.
struct ConnectionDispatcherOptions
{
    /// @brief Number of long-lived worker threads.
    std::size_t worker_count{};

    /// @brief Maximum number of accepted connections waiting for a worker.
    std::size_t pending_connection_limit{};
};

/// @brief Groups dispatcher limits and callbacks into one extensible configuration.
struct ConnectionDispatcherConfig
{
    /// @brief Fixed worker and queue limits.
    ConnectionDispatcherOptions options;

    /// @brief Required callable that exclusively owns each dispatched connection.
    ConnectionHandler handler;

    /// @brief Optional callback invoked after a handler failure is isolated.
    ConnectionFailureObserver failure_observer;
};

/// @brief Dispatches accepted TCP connections through a bounded fixed worker pool.
///
/// The dispatcher owns every successfully submitted connection until a worker
/// transfers it to the configured handler. The class is safe for concurrent
/// submissions, but only one thread may move or destroy the dispatcher itself.
///
/// @warning Destruction, move construction, and move assignment must not run
/// concurrently with `submit()` or `request_stop()` on an affected dispatcher.
/// Call `request_stop()`, wait for that call to return, and then join every
/// producer thread before moving or destroying a dispatcher.
class ConnectionDispatcher final
{
  public:
    /// @brief Creates and starts a dispatcher after validating its configuration.
    /// @param[in] config Resource limits and callbacks transferred to the dispatcher.
    /// @return A running dispatcher, or a structured configuration/startup error.
    [[nodiscard]] static Result<ConnectionDispatcher, DispatchError>
    create(ConnectionDispatcherConfig config);

    /// @brief Requests shutdown, releases pending connections, and joins all workers.
    /// @pre Destruction does not run from one of this dispatcher's handler or
    /// failure-observer callbacks.
    /// @pre No thread is executing `submit()` or `request_stop()` on this dispatcher.
    ~ConnectionDispatcher();

    /// @brief Transfers dispatcher ownership without relocating active worker state.
    ///
    /// The moved-from dispatcher has no implementation: `submit()` returns
    /// `DispatchErrorCode::stopped`, and `request_stop()` has no effect.
    /// @param[in,out] other Dispatcher whose implementation is transferred.
    /// @pre No thread is executing `submit()` or `request_stop()` on `other`.
    ConnectionDispatcher(ConnectionDispatcher &&other) noexcept;

    /// @brief Stops the current dispatcher before taking ownership from another.
    ///
    /// The moved-from dispatcher has no implementation: `submit()` returns
    /// `DispatchErrorCode::stopped`, and `request_stop()` has no effect.
    /// @param[in,out] other Dispatcher whose implementation is transferred.
    /// @return This dispatcher after the ownership transfer.
    /// @pre No thread is executing `submit()` or `request_stop()` on this dispatcher or
    /// `other`.
    /// @pre The call does not run from one of this dispatcher's handler or
    /// failure-observer callbacks.
    ConnectionDispatcher &operator=(ConnectionDispatcher &&other) noexcept;

    /// @brief Copying is forbidden because a dispatcher uniquely owns its workers.
    ConnectionDispatcher(const ConnectionDispatcher &) = delete;
    /// @brief Copy assignment is forbidden because workers have one owner.
    ConnectionDispatcher &operator=(const ConnectionDispatcher &) = delete;

    /// @brief Waits for bounded queue capacity and transfers one connection.
    ///
    /// A successful call gives the dispatcher exclusive ownership of `connection`.
    /// If the queue is full, the call blocks without busy-waiting until capacity,
    /// cancellation, or dispatcher shutdown becomes observable.
    /// @param[in] connection Open connection transferred by value.
    /// @param[in] stop_token Token that can cancel only this submission wait.
    /// @return Success after enqueueing, or a structured submission error.
    [[nodiscard]] Result<void, DispatchError> submit(TcpConnection connection,
                                                     const std::stop_token &stop_token);

    /// @brief Prevents new submissions and requests cancellation of active handlers.
    ///
    /// The function is idempotent and non-blocking. Pending connections are closed
    /// immediately through RAII; active handlers receive their worker stop token.
    /// @pre The call does not run concurrently with destruction, move construction,
    /// or move assignment involving this dispatcher.
    void request_stop() noexcept;

  private:
    /// @brief Concurrent implementation kept stable while the public object moves.
    struct Impl;

    /// @brief Wraps a fully started implementation.
    /// @param[in] impl Owned implementation transferred to the public object.
    explicit ConnectionDispatcher(std::unique_ptr<Impl> impl) noexcept;

    /// @brief Owned dispatcher state, or null after a move.
    std::unique_ptr<Impl> impl_;
};

} // namespace sparenode::network
