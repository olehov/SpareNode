#include "sparenode/network/connection_dispatcher.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"
#include "sparenode/network/detail/tcp_impl.hpp"

namespace sparenode::network
{
namespace
{
class AdmissionStartFailure final : public std::exception
{
  public:
    explicit AdmissionStartFailure(const int native_code) noexcept : native_code_(native_code)
    {
    }

    [[nodiscard]] int native_code() const noexcept
    {
        return native_code_;
    }

  private:
    int native_code_{};
};
} // namespace

/// @brief Stores the bounded queue, callbacks, and fixed worker pool.
struct ConnectionDispatcher::Impl final
{
    /// @brief Owns one accepted connection while its initial protocol prefix is incomplete.
    struct AdmissionEntry
    {
        TcpConnection connection;      ///< Socket retained outside request workers.
        std::vector<std::byte> prefix; ///< Consumed bytes replayed after dispatch.
        NetworkDeadline last_progress; ///< Latest observed prefix growth.
        NetworkDeadline accepted;      ///< Non-renewable admission start.
    };

    /// @brief Allocates the queue once and starts the configured worker count.
    /// @param[in] config Dispatcher limits and callbacks transferred into storage.
    explicit Impl(ConnectionDispatcherConfig config)
        : config_(std::move(config)),
          admission_options_(config_.admission.value_or(ConnectionAdmissionOptions{})),
          admission_enabled_(config_.admission.has_value()),
          ready_queue_(config_.options.pending_connection_limit),
          accepted_queue_(config_.options.pending_connection_limit),
          admissions_(admission_enabled_ ? config_.options.pending_connection_limit : 0)
    {
        if (admission_enabled_)
        {
            const auto runtime = detail::ensure_socket_runtime();
            if (!runtime)
            {
                throw AdmissionStartFailure(runtime.error().code);
            }
            const auto initialized = admission_wake_.ensure_initialized(NetworkOperation::receive);
            if (!initialized)
            {
                throw AdmissionStartFailure(initialized.error().code);
            }
            admission_buffer_.resize(admission_options_.max_prefix_bytes);
            admission_poll_entries_.reserve(config_.options.pending_connection_limit + 1);
            admission_indices_.reserve(config_.options.pending_connection_limit);
        }
        workers_.reserve(config_.options.worker_count);
        for (std::size_t index = 0; index < config_.options.worker_count; ++index)
        {
            workers_.emplace_back([this](const std::stop_token &stop_token)
                                  { worker_loop(stop_token); });
        }
        if (admission_enabled_)
        {
            admission_thread_ = std::jthread([this](const std::stop_token &stop_token)
                                             { admission_loop(stop_token); });
        }
    }

    /// @brief Stops and joins workers before their shared synchronization state is destroyed.
    ~Impl()
    {
        request_stop();
        if (admission_thread_.joinable())
        {
            admission_thread_.join();
        }
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
            lock, stop_token, [this] { return stopping_ || owned_count_ < capacity(); });
        if (!ready)
        {
            return unexpected(DispatchError{DispatchErrorCode::cancelled, 0});
        }
        if (stopping_)
        {
            return unexpected(DispatchError{DispatchErrorCode::stopped, 0});
        }

        ++owned_count_;
        if (admission_enabled_)
        {
            const std::size_t tail = (accepted_head_ + accepted_count_) % accepted_queue_.size();
            accepted_queue_[tail].emplace(std::move(connection));
            ++accepted_count_;
            lock.unlock();
            admission_wake_.notify();
            return {};
        }
        enqueue_ready(std::move(connection));
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
            for (auto &pending_connection : ready_queue_)
            {
                pending_connection.reset();
            }
            for (auto &accepted_connection : accepted_queue_)
            {
                accepted_connection.reset();
            }
            ready_count_ = 0;
            ready_head_ = 0;
            accepted_count_ = 0;
            accepted_head_ = 0;
            owned_count_ = 0;
        }

        admission_thread_.request_stop();
        admission_wake_.notify();
        for (auto &worker : workers_)
        {
            worker.request_stop();
        }
        connection_available_.notify_all();
        space_available_.notify_all();
    }

  private:
    /// @brief Returns the fixed total accepted-connection capacity.
    /// @return Maximum sockets waiting across admission and ready queues.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return ready_queue_.size();
    }

    /// @brief Appends a worker-ready connection while the queue mutex is held.
    /// @param[in] connection Socket transferred into the ready ring buffer.
    void enqueue_ready(TcpConnection connection)
    {
        const std::size_t tail = (ready_head_ + ready_count_) % ready_queue_.size();
        ready_queue_[tail].emplace(std::move(connection));
        ++ready_count_;
    }

    /// @brief Removes the oldest pending connection while the queue mutex is held.
    /// @return The connection transferred out of the ring buffer.
    [[nodiscard]] TcpConnection dequeue()
    {
        auto &occupied_slot = ready_queue_[ready_head_];
        if (!occupied_slot.has_value())
        {
            // A positive pending count guarantees that the head slot is occupied.
            std::terminate();
        }

        TcpConnection connection = std::move(occupied_slot.value());
        occupied_slot.reset();
        ready_head_ = (ready_head_ + 1) % ready_queue_.size();
        --ready_count_;
        --owned_count_;
        return connection;
    }

    /// @brief Moves newly submitted sockets into admission-thread-owned slots.
    /// @return `false` after dispatcher shutdown begins.
    [[nodiscard]] bool drain_accepted()
    {
        std::scoped_lock lock(mutex_);
        if (stopping_)
        {
            return false;
        }
        while (accepted_count_ > 0)
        {
            const auto free_slot = std::ranges::find_if(admissions_, [](const auto &entry)
                                                        { return !entry.has_value(); });
            if (free_slot == admissions_.end())
            {
                std::terminate();
            }
            auto &accepted = accepted_queue_[accepted_head_];
            if (!accepted.has_value())
            {
                std::terminate();
            }
            const auto started = accepted->accepted_at();
            auto owned = std::exchange(accepted, std::nullopt);
            if (!owned.has_value())
            {
                std::terminate();
            }
            free_slot->emplace(AdmissionEntry{std::move(owned).value(), {}, started, started});
            accepted_head_ = (accepted_head_ + 1) % accepted_queue_.size();
            --accepted_count_;
        }
        return true;
    }

    /// @brief Calculates the earliest admission timeout across active sockets.
    /// @return Earliest inactivity or total deadline, or no deadline without active sockets.
    [[nodiscard]] std::optional<NetworkDeadline> next_admission_deadline() const noexcept
    {
        std::optional<NetworkDeadline> result;
        const auto &options = admission_options_;
        for (const auto &entry : admissions_)
        {
            if (!entry.has_value())
            {
                continue;
            }
            const auto deadline = (std::min)(entry->last_progress + options.inactivity_timeout,
                                             entry->accepted + options.total_timeout);
            result = result.has_value() ? (std::min)(result.value(), deadline) : deadline;
        }
        return result;
    }

    /// @brief Rebuilds the native poll list and its admission-slot mapping.
    void prepare_admission_poll()
    {
        admission_poll_entries_.clear();
        admission_indices_.clear();
        detail::SocketPollEntry wake_entry{};
        wake_entry.socket = admission_wake_.reader();
        wake_entry.watch_readable = true;
        admission_poll_entries_.push_back(wake_entry);
        for (std::size_t index = 0; index < admissions_.size(); ++index)
        {
            auto &slot = admissions_[index];
            if (!slot.has_value())
            {
                continue;
            }
            detail::SocketPollEntry connection_entry{};
            connection_entry.socket = slot.value().connection.impl_->socket;
            connection_entry.watch_readable = true;
            admission_poll_entries_.push_back(connection_entry);
            admission_indices_.push_back(index);
        }
    }

    /// @brief Transfers an admitted socket to the request-worker queue.
    /// @param[in] index Admission slot whose socket is ready or expired.
    void promote(const std::size_t index)
    {
        auto &entry = admissions_[index];
        if (!entry.has_value())
        {
            return;
        }
        entry->connection.impl_->prefetched = std::move(entry->prefix);
        entry->connection.impl_->prefetched_offset = 0;
        TcpConnection connection = std::move(entry->connection);
        entry.reset();
        {
            std::scoped_lock lock(mutex_);
            if (stopping_)
            {
                return;
            }
            enqueue_ready(std::move(connection));
        }
        connection_available_.notify_one();
    }

    /// @brief Reports whether a consumed bounded prefix contains the configured marker.
    /// @param[in] entry Admission state containing the accumulated prefix.
    /// @param[in] previous_size Prefix size before the most recent receive.
    /// @return `true` when the completion marker crosses or follows the new suffix.
    [[nodiscard]] bool prefix_complete(const AdmissionEntry &entry,
                                       const std::size_t previous_size) const noexcept
    {
        const auto prefix = std::string_view(reinterpret_cast<const char *>(entry.prefix.data()),
                                             entry.prefix.size());
        const auto overlap = admission_options_.completion_marker.size() - 1;
        const auto search_from = previous_size > overlap ? previous_size - overlap : 0;
        return prefix.find(admission_options_.completion_marker, search_from) !=
               std::string_view::npos;
    }

    /// @brief Consumes one bounded prefix chunk for later replay by the connection.
    /// @param[in] index Readable admission slot to advance.
    void inspect_admission(const std::size_t index)
    {
        auto &entry = admissions_[index];
        if (!entry.has_value())
        {
            return;
        }
        while (true)
        {
            const auto remaining = admission_options_.max_prefix_bytes - entry->prefix.size();
            const auto requested = (std::min)(remaining, admission_buffer_.size());
            const auto received = detail::receive_socket(
                entry->connection.impl_->socket, std::span(admission_buffer_).first(requested));
            if (received >= 0)
            {
                const auto size = static_cast<std::size_t>(received);
                const auto previous_size = entry->prefix.size();
                if (size > 0)
                {
                    entry->prefix.insert(entry->prefix.end(), admission_buffer_.begin(),
                                         admission_buffer_.begin() +
                                             static_cast<std::ptrdiff_t>(size));
                    entry->last_progress = std::chrono::steady_clock::now();
                    entry->connection.impl_->last_receive_progress = entry->last_progress;
                }
                if (size == 0 || entry->prefix.size() == admission_options_.max_prefix_bytes ||
                    prefix_complete(entry.value(), previous_size))
                {
                    promote(index);
                }
                return;
            }
            const int error = detail::last_socket_error();
            if (detail::socket_error_interrupted(error))
            {
                continue;
            }
            if (!detail::socket_error_would_block(error))
            {
                promote(index);
            }
            return;
        }
    }

    /// @brief Sends expired admissions to workers so normal timeout reporting remains intact.
    void promote_expired()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto &options = admission_options_;
        for (std::size_t index = 0; index < admissions_.size(); ++index)
        {
            const auto &entry = admissions_[index];
            if (entry.has_value() && (now >= entry->last_progress + options.inactivity_timeout ||
                                      now >= entry->accepted + options.total_timeout))
            {
                promote(index);
            }
        }
    }

    /// @brief Transfers every active admission after a poller-wide failure.
    void promote_all_admissions()
    {
        for (std::size_t index = 0; index < admissions_.size(); ++index)
        {
            promote(index);
        }
    }

    /// @brief Handles wake, socket, and timeout results from one completed poll.
    void process_admission_events()
    {
        if (admission_poll_entries_.front().readable)
        {
            admission_wake_.drain();
        }
        for (std::size_t poll_index = 1; poll_index < admission_poll_entries_.size(); ++poll_index)
        {
            const auto &event = admission_poll_entries_[poll_index];
            if (event.readable || event.error || event.hangup || event.invalid)
            {
                inspect_admission(admission_indices_[poll_index - 1]);
            }
        }
        promote_expired();
    }

    /// @brief Polls all incomplete prefixes on one bounded admission thread.
    /// @param[in] stop_token Token requested during dispatcher shutdown.
    void admission_loop(const std::stop_token &stop_token) noexcept
    {
        try
        {
            while (!stop_token.stop_requested() && drain_accepted())
            {
                prepare_admission_poll();
                auto waited = admission_poller_.wait(
                    admission_poll_entries_, NetworkOperation::receive, next_admission_deadline());
                if (!waited)
                {
                    promote_all_admissions();
                    continue;
                }
                process_admission_events();
            }
        }
        catch (...)
        {
            request_stop();
        }
        for (auto &entry : admissions_)
        {
            entry.reset();
        }
    }

    /// @brief Waits for and processes connections until dispatcher shutdown.
    /// @param[in] stop_token Token requested by `request_stop()`.
    void worker_loop(const std::stop_token &stop_token) noexcept
    {
        while (!stop_token.stop_requested())
        {
            std::unique_lock lock(mutex_);
            const bool ready = connection_available_.wait(
                lock, stop_token, [this] { return stopping_ || ready_count_ > 0; });
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
    /// @brief Value-initialized admission policy used only when enabled.
    ConnectionAdmissionOptions admission_options_;
    /// @brief Whether accepted sockets pass through the admission poller.
    bool admission_enabled_{};
    /// @brief Protects queue indices, queue entries, and shutdown state.
    std::mutex mutex_;
    /// @brief Wakes consumers when work arrives or shutdown begins.
    std::condition_variable_any connection_available_;
    /// @brief Wakes producers when capacity opens or shutdown begins.
    std::condition_variable_any space_available_;
    /// @brief Preallocated ring buffer of connections ready for request workers.
    std::vector<std::optional<TcpConnection>> ready_queue_;
    /// @brief Preallocated ring buffer transferring accepted sockets to admission.
    std::vector<std::optional<TcpConnection>> accepted_queue_;
    /// @brief Admission-thread-owned sockets with incomplete prefixes.
    std::vector<std::optional<AdmissionEntry>> admissions_;
    /// @brief Reusable native readiness list containing wake and connection sockets.
    std::vector<detail::SocketPollEntry> admission_poll_entries_;
    /// @brief Maps connection poll entries back to admission slots.
    std::vector<std::size_t> admission_indices_;
    /// @brief One bounded scratch buffer reused while consuming admission prefixes.
    std::vector<std::byte> admission_buffer_;
    /// @brief Polls every incomplete socket on the admission thread.
    detail::NativeSocketPoller admission_poller_;
    /// @brief Interrupts admission polling for submissions and shutdown.
    detail::SocketWakeChannel admission_wake_;
    std::size_t ready_head_{};     ///< Oldest request-worker-ready slot.
    std::size_t ready_count_{};    ///< Number of request-worker-ready sockets.
    std::size_t accepted_head_{};  ///< Oldest socket awaiting admission ownership.
    std::size_t accepted_count_{}; ///< Number of sockets awaiting admission ownership.
    std::size_t owned_count_{};    ///< Total sockets across all bounded dispatcher stages.
    /// @brief Indicates that no more connections may be accepted.
    bool stopping_{};                   ///< Prevents submissions and wakes every managed stage.
    std::jthread admission_thread_;     ///< Keeps incomplete prefixes off request workers.
    std::vector<std::jthread> workers_; ///< Fixed request workers, declared last for safe joining.
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
    if (config.admission.has_value() &&
        (config.admission->max_prefix_bytes == 0 || config.admission->completion_marker.empty() ||
         config.admission->completion_marker.size() > config.admission->max_prefix_bytes ||
         config.admission->inactivity_timeout <= std::chrono::milliseconds::zero() ||
         config.admission->total_timeout <= std::chrono::milliseconds::zero()))
    {
        return unexpected(DispatchError{DispatchErrorCode::invalid_admission_config, 0});
    }

    try
    {
        return ConnectionDispatcher(std::make_unique<Impl>(std::move(config)));
    }
    catch (const AdmissionStartFailure &error)
    {
        return unexpected(
            DispatchError{DispatchErrorCode::admission_start_failed, error.native_code()});
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
