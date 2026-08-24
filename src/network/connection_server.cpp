#include "sparenode/network/connection_server.hpp"

#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

#include "sparenode/network/tcp_listener.hpp"

namespace sparenode::network
{

/// @brief Owns the listener, dispatcher, observer, and cancellable accept thread.
struct ConnectionServer::Impl final
{
    /// @brief Starts the accept loop after every resource it uses is initialized.
    /// @param[in] listener Bound listener transferred into the server.
    /// @param[in] endpoint Cached endpoint safe to read while accept is active.
    /// @param[in] dispatcher Started dispatcher transferred into the server.
    /// @param[in] failure_observer Optional accept-loop failure observer.
    Impl(TcpListener listener, TcpEndpoint endpoint, ConnectionDispatcher dispatcher,
         ConnectionServerFailureObserver failure_observer)
        : listener_(std::move(listener)), endpoint_(std::move(endpoint)),
          dispatcher_(std::move(dispatcher)), failure_observer_(std::move(failure_observer)),
          accept_thread_([this](const std::stop_token &stop_token) { run_accept_loop(stop_token); })
    {
    }

    /// @brief Stops and joins every managed thread before shared state is destroyed.
    ~Impl()
    {
        request_stop();
        accept_thread_.join();
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    /// @brief Returns the endpoint cached before the accept thread started.
    /// @return Stable numeric endpoint bound by the listener.
    [[nodiscard]] const TcpEndpoint &local_endpoint() const noexcept
    {
        return endpoint_;
    }

    /// @brief Cancels the accept loop, blocked submissions, and active handlers.
    void request_stop() noexcept
    {
        accept_thread_.request_stop();
        dispatcher_.request_stop();
    }

  private:
    /// @brief Runs the accept loop while preventing exceptions from escaping the thread boundary.
    /// @param[in] stop_token Token used to request server shutdown.
    void run_accept_loop(const std::stop_token &stop_token) noexcept
    {
        try
        {
            accept_loop(stop_token);
        }
        catch (...)
        {
            stop_after_failure(ConnectionServerFailure{
                ConnectionServerFailureKind::internal_exception, std::nullopt, std::nullopt});
        }
    }

    /// @brief Accepts clients until cancellation or a fatal listener failure.
    /// @param[in] stop_token Token requested during server shutdown.
    void accept_loop(const std::stop_token &stop_token)
    {
        while (!stop_token.stop_requested())
        {
            auto connection = listener_.accept(stop_token);
            if (!connection)
            {
                if (stop_token.stop_requested() &&
                    connection.error().domain == NetworkErrorDomain::cancellation)
                {
                    return;
                }

                stop_after_failure(ConnectionServerFailure{
                    ConnectionServerFailureKind::accept_error, connection.error(), std::nullopt});
                return;
            }

            auto submission = dispatcher_.submit(std::move(connection).value(), stop_token);
            if (!submission)
            {
                if (stop_token.stop_requested() &&
                    (submission.error().code == DispatchErrorCode::cancelled ||
                     submission.error().code == DispatchErrorCode::stopped))
                {
                    return;
                }

                stop_after_failure(ConnectionServerFailure{
                    ConnectionServerFailureKind::dispatch_error, std::nullopt, submission.error()});
                return;
            }
        }
    }

    /// @brief Stops connection workers before reporting a fatal accept-loop failure.
    /// @param[in] failure Failure that permanently ended the accept loop.
    void stop_after_failure(const ConnectionServerFailure &failure) noexcept
    {
        dispatcher_.request_stop();
        notify_failure(failure);
    }

    /// @brief Invokes the optional lifecycle observer while containing exceptions.
    /// @param[in] failure Failure visible only for the duration of the callback.
    void notify_failure(const ConnectionServerFailure &failure) noexcept
    {
        if (!failure_observer_)
        {
            return;
        }

        try
        {
            failure_observer_(failure);
        }
        catch (...)
        {
            // Diagnostics must not escape the accept-thread boundary.
            return;
        }
    }

    /// @brief Listener used exclusively by the accept thread after startup.
    TcpListener listener_;
    /// @brief Bound endpoint cached before listener operations become concurrent.
    TcpEndpoint endpoint_;
    /// @brief Fixed worker pool and bounded pending-connection queue.
    ConnectionDispatcher dispatcher_;
    /// @brief Optional observer for accept and dispatch boundary failures.
    ConnectionServerFailureObserver failure_observer_;
    /// @brief Single producer that feeds accepted connections to the dispatcher.
    std::jthread accept_thread_;
};

// Wraps an implementation whose accept thread and dispatcher are already running.
ConnectionServer::ConnectionServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Result<ConnectionServer, ConnectionServerStartError>
ConnectionServer::start(ConnectionServerConfig config)
{
    try
    {
        auto listener = TcpListener::bind(config.endpoint, config.listen_backlog);
        if (!listener)
        {
            return unexpected(
                ConnectionServerStartError{ConnectionServerStartErrorCode::listener_start_failed,
                                           listener.error(), std::nullopt, 0});
        }

        auto endpoint = listener->local_endpoint();
        if (!endpoint)
        {
            return unexpected(
                ConnectionServerStartError{ConnectionServerStartErrorCode::listener_start_failed,
                                           endpoint.error(), std::nullopt, 0});
        }

        if (!config.multithreading_enabled)
        {
            config.dispatcher.options.worker_count = 1;
        }

        auto dispatcher = ConnectionDispatcher::create(std::move(config.dispatcher));
        if (!dispatcher)
        {
            return unexpected(
                ConnectionServerStartError{ConnectionServerStartErrorCode::dispatcher_start_failed,
                                           std::nullopt, dispatcher.error(), 0});
        }

        return ConnectionServer(std::make_unique<Impl>(
            std::move(listener).value(), std::move(endpoint).value(), std::move(dispatcher).value(),
            std::move(config.failure_observer)));
    }
    catch (const std::system_error &error)
    {
        return unexpected(
            ConnectionServerStartError{ConnectionServerStartErrorCode::accept_thread_start_failed,
                                       std::nullopt, std::nullopt, error.code().value()});
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            ConnectionServerStartError{ConnectionServerStartErrorCode::resource_allocation_failed,
                                       std::nullopt, std::nullopt, 0});
    }
    catch (const std::length_error &)
    {
        return unexpected(
            ConnectionServerStartError{ConnectionServerStartErrorCode::resource_allocation_failed,
                                       std::nullopt, std::nullopt, 0});
    }
}

// Stops all active work before releasing the stable implementation.
ConnectionServer::~ConnectionServer() = default;

// Transfers only the implementation pointer; managed threads remain in place.
ConnectionServer::ConnectionServer(ConnectionServer &&) noexcept = default;

// Destroys any current implementation before taking ownership of the source.
ConnectionServer &ConnectionServer::operator=(ConnectionServer &&) noexcept = default;

std::optional<TcpEndpoint> ConnectionServer::local_endpoint() const
{
    if (!impl_)
    {
        return std::nullopt;
    }
    return impl_->local_endpoint();
}

// Delegates cooperative cancellation to the stable implementation.
void ConnectionServer::request_stop() noexcept
{
    if (impl_)
    {
        impl_->request_stop();
    }
}

} // namespace sparenode::network
