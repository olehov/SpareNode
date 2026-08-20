#include "sparenode/network/connection_dispatcher.hpp"

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace sparenode::network
{

/// @brief Stores the bounded queue, callbacks, and fixed worker pool.
struct ConnectionDispatcher::Impl final
{
    /// @brief Allocates the queue once and starts the configured worker count.
    /// @param[in] config Dispatcher limits and callbacks transferred into storage.
    explicit Impl(ConnectionDispatcherConfig config)
        : config_(std::move(config)), queue_(config_.options.pending_connection_limit)
    {
        workers_.reserve(config_.options.worker_count);
        for (std::size_t index = 0; index < config_.options.worker_count; ++index)
        {
            workers_.emplace_back([this](const std::stop_token &stop_token)
                                  { worker_loop(stop_token); });
        }
    }

    /// @brief Stops and joins workers before their shared synchronization state is destroyed.
    ~Impl()
    {
        request_stop();
        workers_.clear();
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    /// @brief Waits for a free ring-buffer slot and transfers one connection into it.
    /// @param[in] connection Connection whose ownership is transferred on success.
    /// @param[in] stop_token Token that cancels this producer wait.
    /// @return Success after enqueueing, or a stopped/cancelled error.
    [[nodiscard]] Result<void, DispatchError> submit(TcpConnection connection,
                                                     const std::stop_token &stop_token)
    {
        if (!connection.is_open())
        {
            return unexpected(DispatchError{DispatchErrorCode::invalid_connection, 0});
        }
        if (stop_token.stop_requested())
        {
            return unexpected(DispatchError{DispatchErrorCode::cancelled, 0});
        }

        std::unique_lock lock(mutex_);
        const bool ready = space_available_.wait(
            lock, stop_token, [this] { return stopping_ || pending_count_ < queue_.size(); });
        if (!ready)
        {
            return unexpected(DispatchError{DispatchErrorCode::cancelled, 0});
        }
        if (stopping_)
        {
            return unexpected(DispatchError{DispatchErrorCode::stopped, 0});
        }

        const std::size_t tail = (head_ + pending_count_) % queue_.size();
        queue_[tail].emplace(std::move(connection));
        ++pending_count_;
        lock.unlock();
        connection_available_.notify_one();
        return {};
    }

    /// @brief Marks shutdown, releases queued connections, and wakes every waiter.
    void request_stop() noexcept
    {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_)
            {
                return;
            }

            stopping_ = true;
            for (auto &pending_connection : queue_)
            {
                pending_connection.reset();
            }
            pending_count_ = 0;
            head_ = 0;
        }

        for (auto &worker : workers_)
        {
            worker.request_stop();
        }
        connection_available_.notify_all();
        space_available_.notify_all();
    }

  private:
    /// @brief Removes the oldest pending connection while the queue mutex is held.
    /// @return The connection transferred out of the ring buffer.
    [[nodiscard]] TcpConnection dequeue()
    {
        auto &occupied_slot = queue_[head_];
        if (!occupied_slot.has_value())
        {
            // A positive pending count guarantees that the head slot is occupied.
            std::terminate();
        }

        TcpConnection connection = std::move(occupied_slot.value());
        occupied_slot.reset();
        head_ = (head_ + 1) % queue_.size();
        --pending_count_;
        return connection;
    }

    /// @brief Waits for and processes connections until dispatcher shutdown.
    /// @param[in] stop_token Token requested by `request_stop()`.
    void worker_loop(const std::stop_token &stop_token) noexcept
    {
        while (!stop_token.stop_requested())
        {
            std::unique_lock lock(mutex_);
            const bool ready = connection_available_.wait(
                lock, stop_token, [this] { return stopping_ || pending_count_ > 0; });
            if (!ready || stopping_)
            {
                return;
            }

            TcpConnection connection = dequeue();
            lock.unlock();
            space_available_.notify_one();
            process(std::move(connection), stop_token);
        }
    }

    /// @brief Runs one handler and converts failures into observer notifications.
    /// @param[in] connection Connection transferred exclusively to the handler.
    /// @param[in] stop_token Worker cancellation token.
    void process(TcpConnection connection, const std::stop_token &stop_token) noexcept
    {
        try
        {
            auto result = config_.handler(std::move(connection), stop_token);
            if (!result)
            {
                notify_failure(
                    ConnectionFailure{ConnectionFailureKind::handler_error, result.error()});
            }
        }
        catch (...)
        {
            notify_failure(
                ConnectionFailure{ConnectionFailureKind::handler_exception, std::nullopt});
        }
    }

    /// @brief Invokes the optional observer while containing observer exceptions.
    /// @param[in] failure Failure visible only for the duration of the callback.
    void notify_failure(const ConnectionFailure &failure) noexcept
    {
        if (!config_.failure_observer)
        {
            return;
        }

        try
        {
            config_.failure_observer(failure);
        }
        catch (...)
        {
            // Observability must never terminate a worker or another connection.
            return;
        }
    }

    /// @brief Immutable limits and callbacks shared by every worker.
    ConnectionDispatcherConfig config_;
    /// @brief Protects queue indices, queue entries, and shutdown state.
    std::mutex mutex_;
    /// @brief Wakes consumers when work arrives or shutdown begins.
    std::condition_variable_any connection_available_;
    /// @brief Wakes producers when capacity opens or shutdown begins.
    std::condition_variable_any space_available_;
    /// @brief Preallocated ring-buffer slots that own pending connections.
    std::vector<std::optional<TcpConnection>> queue_;
    /// @brief Index of the oldest occupied ring-buffer slot.
    std::size_t head_{};
    /// @brief Number of occupied slots in the bounded queue.
    std::size_t pending_count_{};
    /// @brief Indicates that no more connections may be accepted.
    bool stopping_{};
    /// @brief Declared last so workers join before preceding shared state is destroyed.
    std::vector<std::jthread> workers_;
};

// Wraps an implementation whose worker threads have already started.
ConnectionDispatcher::ConnectionDispatcher(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

// Validates fixed limits before allocating resources or starting threads.
Result<ConnectionDispatcher, DispatchError>
ConnectionDispatcher::create(ConnectionDispatcherConfig config)
{
    if (config.options.worker_count == 0)
    {
        return unexpected(DispatchError{DispatchErrorCode::invalid_worker_count, 0});
    }
    if (config.options.pending_connection_limit == 0)
    {
        return unexpected(DispatchError{DispatchErrorCode::invalid_pending_connection_limit, 0});
    }
    if (!config.handler)
    {
        return unexpected(DispatchError{DispatchErrorCode::missing_connection_handler, 0});
    }

    try
    {
        return ConnectionDispatcher(std::make_unique<Impl>(std::move(config)));
    }
    catch (const std::system_error &error)
    {
        return unexpected(
            DispatchError{DispatchErrorCode::worker_start_failed, error.code().value()});
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(DispatchError{DispatchErrorCode::resource_allocation_failed, 0});
    }
    catch (const std::length_error &)
    {
        return unexpected(DispatchError{DispatchErrorCode::resource_allocation_failed, 0});
    }
}

// Stops active work through the implementation before releasing shared state.
ConnectionDispatcher::~ConnectionDispatcher() = default;

// Transfers only the stable implementation pointer; active workers are not relocated.
ConnectionDispatcher::ConnectionDispatcher(ConnectionDispatcher &&) noexcept = default;

// Destroys any current implementation before taking ownership of the source state.
ConnectionDispatcher &ConnectionDispatcher::operator=(ConnectionDispatcher &&) noexcept = default;

// Delegates the stop-aware bounded enqueue operation to the shared implementation.
Result<void, DispatchError> ConnectionDispatcher::submit(TcpConnection connection,
                                                         const std::stop_token &stop_token)
{
    if (!impl_)
    {
        return unexpected(DispatchError{DispatchErrorCode::stopped, 0});
    }
    return impl_->submit(std::move(connection), stop_token);
}

// Begins cooperative shutdown without waiting for active handlers to return.
void ConnectionDispatcher::request_stop() noexcept
{
    if (impl_)
    {
        impl_->request_stop();
    }
}

} // namespace sparenode::network
