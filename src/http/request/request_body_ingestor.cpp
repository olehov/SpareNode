#include "sparenode/http/request/request_body_ingestor.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

#include "sparenode/http/request/detail/body_decode_buffer.hpp"

namespace sparenode::http
{
namespace
{

using FileOperationResult = Result<void, filesystem::TemporaryFileError>;

/// @brief Converts worker creation failures into the existing resource error category.
[[nodiscard]] FileOperationResult file_worker_unavailable(const int native_code = 0)
{
    return unexpected(filesystem::TemporaryFileError{
        filesystem::TemporaryFileErrorCode::resource_allocation_failed, native_code});
}

/// @brief Bounds active native file operations, including those orphaned by timeouts.
class FileWorkerSlots
{
  public:
    struct Ticket
    {
        bool orphaned{}; ///< A request stopped waiting during this native operation.
        bool released{}; ///< Prevents accounting the same worker twice.
    };

    /// @brief Waits for capacity while respecting the request deadline and cancellation.
    [[nodiscard]] Result<std::shared_ptr<Ticket>, filesystem::TemporaryFileError>
    acquire(const filesystem::TemporaryFileIoOptions &options)
    {
        auto ticket = std::make_shared<Ticket>();
        std::unique_lock lock(mutex_);
        const auto available = [this]
        { return active_ < maximum_workers || orphaned_ == maximum_workers; };
        if (options.deadline.has_value())
        {
            static_cast<void>(available_.wait_until(lock, options.stop_token,
                                                    options.deadline.value(), available));
        }
        else
        {
            static_cast<void>(available_.wait(lock, options.stop_token, available));
        }
        if (options.stop_token.stop_requested())
        {
            return unexpected(
                filesystem::TemporaryFileError{filesystem::TemporaryFileErrorCode::cancelled, 0});
        }
        if (options.deadline.has_value() &&
            std::chrono::steady_clock::now() >= options.deadline.value())
        {
            return unexpected(filesystem::TemporaryFileError{
                filesystem::TemporaryFileErrorCode::deadline_exceeded, 0});
        }
        if (orphaned_ == maximum_workers)
        {
            return unexpected(filesystem::TemporaryFileError{
                filesystem::TemporaryFileErrorCode::resource_allocation_failed, 0});
        }
        ++active_;
        return ticket;
    }

    /// @brief Marks a detached worker without freeing its occupied capacity.
    void orphan(const std::shared_ptr<Ticket> &ticket) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!ticket->released && !ticket->orphaned)
        {
            ticket->orphaned = true;
            ++orphaned_;
            available_.notify_all();
        }
    }

    /// @brief Returns capacity after native I/O and any deferred cleanup finish.
    void release(const std::shared_ptr<Ticket> &ticket) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!ticket->released)
        {
            ticket->released = true;
            --active_;
            if (ticket->orphaned)
            {
                --orphaned_;
            }
            available_.notify_all();
        }
    }

  private:
    /// 32 isolates storage stalls without reserving slots for idle file workers.
    static constexpr std::size_t maximum_workers = 32;
    std::mutex mutex_;                      ///< Protects worker and orphan counts.
    std::condition_variable_any available_; ///< Wakes queued requests after release.
    std::size_t active_{};                  ///< Native operations currently owning a slot.
    std::size_t orphaned_{};                ///< Active operations detached by a request.
};

/// @brief Keeps slot synchronization alive until process exit, including detached workers.
[[nodiscard]] FileWorkerSlots &file_worker_slots()
{
    static auto *slots = new FileWorkerSlots();
    return *slots;
}

/// @brief Converts active policy state into a terminal ingestion error when interrupted.
[[nodiscard]] std::optional<RequestBodyIngestionError>
interruption(const filesystem::TemporaryFileIoOptions &options) noexcept
{
    if (options.stop_token.stop_requested())
    {
        return RequestBodyIngestionError{RequestBodyIngestionErrorCode::cancelled, {}, {}};
    }
    if (options.deadline.has_value() &&
        std::chrono::steady_clock::now() >= options.deadline.value())
    {
        return RequestBodyIngestionError{RequestBodyIngestionErrorCode::deadline_exceeded, {}, {}};
    }
    return std::nullopt;
}

} // namespace

/// @brief Reuses one native I/O thread and one fixed payload buffer for a request.
struct RequestBodyIngestor::FileWorker
{
    /// @brief Selects a payload append or final file flush.
    enum class Operation : std::uint8_t
    {
        write,   ///< Append the currently queued payload bytes.
        complete ///< Flush and close the file after the final body bytes.
    };

    /// @brief Keeps file ownership and queued bytes independent of the request worker.
    struct State
    {
        /// @brief Retains the artifact until the worker finishes, including after timeout.
        /// @param[in] file Request-owned artifact transferred to shared worker ownership.
        explicit State(std::shared_ptr<filesystem::TemporaryFile> file) : file(std::move(file))
        {
        }

        std::mutex mutex;                    ///< Protects queued work and result publication.
        std::condition_variable_any changed; ///< Wakes the file and request workers.
        std::shared_ptr<filesystem::TemporaryFile> file; ///< Owns delayed cleanup.
        std::shared_ptr<FileWorkerSlots::Ticket> ticket; ///< Slot for the current operation.
        std::array<std::byte, detail::body_decode_buffer_bytes> buffer{}; ///< Reused payload.
        filesystem::TemporaryFileIoOptions options{}; ///< Policy for the queued operation.
        std::optional<FileOperationResult> result;    ///< Result of the latest operation.
        std::size_t bytes{};                          ///< Payload length currently in the buffer.
        std::uint64_t sequence{};                     ///< Latest queued operation identifier.
        std::uint64_t completed{};                    ///< Latest finished operation identifier.
        Operation operation{Operation::write};        ///< Kind of the queued operation.
        bool pending{};                               ///< One operation is queued or running.
        bool stopping{};                              ///< Worker exits after the current operation.
    };

    /// @brief Owns one persistent I/O thread's shared state.
    /// @param[in] state Shared operation queue and file ownership.
    explicit FileWorker(std::shared_ptr<State> state) : state(std::move(state))
    {
    }

    /// @brief Leaves an unfinished native operation with its shared worker state.
    ~FileWorker()
    {
        if (thread.joinable())
        {
            abandon();
        }
    }

    /// @brief Processes successive writes and finalization without replacing the thread.
    /// @param[in] state Shared operation queue and file ownership.
    static void work(const std::shared_ptr<State> &state) noexcept
    {
        while (true)
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&state] { return state->pending || state->stopping; });
            if (!state->pending)
            {
                break;
            }
            const auto operation = state->operation;
            const auto bytes = state->bytes;
            const auto options = state->options;
            const auto sequence = state->sequence;
            lock.unlock();

            FileOperationResult result;
            try
            {
                result = operation == Operation::write
                             ? state->file->write(std::span(state->buffer).first(bytes), options)
                             : state->file->complete(options);
            }
            catch (...)
            {
                result = file_worker_unavailable();
            }

            lock.lock();
            state->result.emplace(result);
            state->completed = sequence;
            state->pending = false;
            const auto ticket = std::move(state->ticket);
            const auto stopping = state->stopping;
            lock.unlock();
            if (stopping)
            {
                state->file.reset();
            }
            file_worker_slots().release(ticket);
            state->changed.notify_all();
        }
        state->file.reset();
    }

    /// @brief Queues one operation and waits only until completion or policy interruption.
    /// @param[in] operation Whether to append bytes or complete the file.
    /// @param[in] payload Decoded bytes for a write, empty for completion.
    /// @param[in] options Cancellation and deadline governing this wait.
    /// @return Native operation result or the first observed interruption.
    [[nodiscard]] FileOperationResult run(const Operation operation,
                                          const std::span<const std::byte> payload,
                                          const filesystem::TemporaryFileIoOptions &options)
    {
        std::shared_ptr<FileWorkerSlots::Ticket> ticket;
        try
        {
            auto acquired = file_worker_slots().acquire(options);
            if (!acquired)
            {
                return unexpected(acquired.error());
            }
            ticket = std::move(acquired).value();
        }
        catch (const std::bad_alloc &)
        {
            return file_worker_unavailable();
        }
        std::unique_lock lock(state->mutex);
        if (state->stopping || state->pending || payload.size() > state->buffer.size())
        {
            file_worker_slots().release(ticket);
            return unexpected(filesystem::TemporaryFileError{
                filesystem::TemporaryFileErrorCode::invalid_state, 0});
        }
        std::copy(payload.begin(), payload.end(), state->buffer.begin());
        state->bytes = payload.size();
        state->operation = operation;
        state->options = options;
        state->ticket = ticket;
        state->result.reset();
        const auto sequence = ++state->sequence;
        state->pending = true;
        lock.unlock();
        state->changed.notify_all();
        lock.lock();

        const auto finished = [&] { return state->completed == sequence; };
        if (options.deadline.has_value())
        {
            static_cast<void>(state->changed.wait_until(lock, options.stop_token,
                                                        options.deadline.value(), finished));
        }
        else
        {
            static_cast<void>(state->changed.wait(lock, options.stop_token, finished));
        }
        if (options.stop_token.stop_requested())
        {
            return unexpected(
                filesystem::TemporaryFileError{filesystem::TemporaryFileErrorCode::cancelled, 0});
        }
        if (options.deadline.has_value() &&
            std::chrono::steady_clock::now() >= options.deadline.value())
        {
            return unexpected(filesystem::TemporaryFileError{
                filesystem::TemporaryFileErrorCode::deadline_exceeded, 0});
        }
        if (!finished())
        {
            return file_worker_unavailable();
        }
        return state->result.value_or(file_worker_unavailable());
    }

    /// @brief Releases the request worker while retaining the file in the I/O state.
    void abandon() noexcept
    {
        std::shared_ptr<FileWorkerSlots::Ticket> ticket;
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
            ticket = state->ticket;
        }
        state->changed.notify_all();
        if (ticket)
        {
            file_worker_slots().orphan(ticket);
        }
        try
        {
            thread.detach();
        }
        catch (const std::system_error &)
        {
            thread.join();
        }
    }

    /// @brief Stops and joins an idle worker after successful ingestion.
    void finish() noexcept
    {
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
        }
        state->changed.notify_all();
        thread.join();
    }

    std::shared_ptr<State> state; ///< Owned until joined or detached completion.
    std::thread thread;           ///< One thread per active request body.
};

/// @brief Initializes framing and retains an optional temporary-file factory.
RequestBodyIngestor::RequestBodyIngestor(const HttpRequestHead &head,
                                         const HttpRequestParserLimits &limits,
                                         TemporaryBodyFileFactory file_factory)
    : decoder_(head, limits), file_factory_(std::move(file_factory))
{
    finalized_ = decoder_.complete();
}

RequestBodyIngestor::~RequestBodyIngestor()
{
    if (file_worker_)
    {
        if (complete())
        {
            file_worker_->finish();
        }
        else
        {
            artifact_.reset();
            file_worker_->abandon();
        }
    }
}

/// @brief Preserves decoder detail while selecting the public ingestion category.
RequestBodyIngestionError
RequestBodyIngestor::framing_failure(const HttpRequestParseError &error) noexcept
{
    return {error.code == HttpRequestParseErrorCode::body_too_large
                ? RequestBodyIngestionErrorCode::body_too_large
                : RequestBodyIngestionErrorCode::framing_failure,
            error,
            {}};
}

/// @brief Maps filesystem policy interruption separately from native storage failures.
RequestBodyIngestionError
RequestBodyIngestor::file_failure(const filesystem::TemporaryFileError &error) noexcept
{
    switch (error.code)
    {
    case filesystem::TemporaryFileErrorCode::cancelled:
        return {RequestBodyIngestionErrorCode::cancelled, {}, error};
    case filesystem::TemporaryFileErrorCode::deadline_exceeded:
        return {RequestBodyIngestionErrorCode::deadline_exceeded, {}, error};
    case filesystem::TemporaryFileErrorCode::resource_allocation_failed:
        return {RequestBodyIngestionErrorCode::resource_failure, {}, error};
    default:
        return {RequestBodyIngestionErrorCode::temporary_file_failure, {}, error};
    }
}

/// @brief Releases partial storage before preserving the terminal ingestion failure.
RequestBodyIngestionError RequestBodyIngestor::fail(const RequestBodyIngestionError &error) noexcept
{
    if (file_worker_)
    {
        file_worker_->abandon();
        file_worker_.reset();
    }
    artifact_.reset();
    failure_ = error;
    return error;
}

/// @brief Starts one reusable I/O worker without reserving a slot between writes.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::ensure_file_worker(const filesystem::TemporaryFileIoOptions &options)
{
    if (file_worker_ || (!options.deadline.has_value() && !options.stop_token.stop_possible()))
    {
        return {};
    }
    try
    {
        auto state = std::make_shared<FileWorker::State>(artifact_);
        auto worker = std::make_unique<FileWorker>(state);
        worker->thread = std::thread([state] { FileWorker::work(state); });
        file_worker_ = std::move(worker);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(fail(file_failure(filesystem::TemporaryFileError{
            filesystem::TemporaryFileErrorCode::resource_allocation_failed, 0})));
    }
    catch (const std::system_error &error)
    {
        return unexpected(fail(file_failure(filesystem::TemporaryFileError{
            filesystem::TemporaryFileErrorCode::resource_allocation_failed,
            error.code().value()})));
    }
}

/// @brief Lazily creates native temporary storage for the first decoded payload bytes.
Result<void, RequestBodyIngestionError> RequestBodyIngestor::ensure_artifact()
{
    if (artifact_)
    {
        return {};
    }
    try
    {
        if (file_factory_)
        {
            auto file = file_factory_();
            if (!file)
            {
                return unexpected(fail(file_failure(file.error())));
            }
            artifact_ = std::move(file).value();
            if (!artifact_)
            {
                return unexpected(fail(RequestBodyIngestionError{
                    RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
            }
            return {};
        }
        auto file = filesystem::TemporaryFile::create();
        if (!file)
        {
            return unexpected(fail(file_failure(file.error())));
        }
        artifact_ = std::make_shared<filesystem::TemporaryFile>(std::move(file).value());
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(fail(
            RequestBodyIngestionError{RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
    }
}

/// @brief Flushes and closes the completed nonempty artifact under active I/O policy.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::finalize(const filesystem::TemporaryFileIoOptions &options)
{
    if (finalized_)
    {
        return {};
    }
    if (artifact_)
    {
        if (auto ready = ensure_file_worker(options); !ready)
        {
            return unexpected(ready.error());
        }
        auto completed = file_worker_
                             ? file_worker_->run(FileWorker::Operation::complete, {}, options)
                             : artifact_->complete(options);
        if (!completed)
        {
            return unexpected(fail(file_failure(completed.error())));
        }
        if (file_worker_)
        {
            file_worker_->finish();
            file_worker_.reset();
        }
    }
    finalized_ = true;
    return {};
}

/// @brief Persists one decoded span under active cancellation and deadline policy.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::persist(const std::span<const std::byte> output,
                             const filesystem::TemporaryFileIoOptions &options)
{
    if (const auto interrupted = interruption(options); interrupted.has_value())
    {
        return unexpected(fail(interrupted.value()));
    }
    if (auto created = ensure_artifact(); !created)
    {
        return unexpected(created.error());
    }
    if (auto ready = ensure_file_worker(options); !ready)
    {
        return unexpected(ready.error());
    }
    auto written = file_worker_ ? file_worker_->run(FileWorker::Operation::write, output, options)
                                : artifact_->write(output, options);
    if (!written)
    {
        return unexpected(fail(file_failure(written.error())));
    }
    body_size_ += output.size();
    return {};
}

/// @brief Decodes fresh wire bytes and persists only emitted payload spans.
Result<HttpBodyDecodeProgress, RequestBodyIngestionError>
RequestBodyIngestor::feed(std::span<const std::byte> input,
                          const filesystem::TemporaryFileIoOptions &options)
{
    if (failure_)
    {
        return unexpected(failure_.value());
    }
    if (const auto interrupted = interruption(options); interrupted.has_value())
    {
        return unexpected(fail(interrupted.value()));
    }
    HttpBodyDecodeProgress total{};
    std::array<std::byte, detail::body_decode_buffer_bytes> output{};
    do
    {
        auto progress = decoder_.feed(input.subspan(total.consumed), output);
        if (!progress)
        {
            return unexpected(fail(framing_failure(progress.error())));
        }
        total.consumed += progress->consumed;
        total.produced += progress->produced;
        total.complete = progress->complete;
        if (progress->produced != 0)
        {
            if (auto persisted = persist(std::span(output).first(progress->produced), options);
                !persisted)
            {
                return unexpected(persisted.error());
            }
        }
        if (progress->complete)
        {
            if (auto completed = finalize(options); !completed)
            {
                return unexpected(completed.error());
            }
            break;
        }
        if (progress->consumed == 0 && progress->produced == 0)
        {
            break;
        }
    } while (total.consumed < input.size());
    return total;
}

/// @brief Rejects unfinished framing or finalizes it under the active I/O policy.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::finish(const filesystem::TemporaryFileIoOptions &options)
{
    if (failure_)
    {
        return unexpected(failure_.value());
    }
    if (const auto interrupted = interruption(options); interrupted.has_value())
    {
        return unexpected(fail(interrupted.value()));
    }
    if (const auto finished = decoder_.finish(); !finished)
    {
        return unexpected(fail(framing_failure(finished.error())));
    }
    return finalize(options);
}

/// @brief Returns stable text for one ingestion failure category.
const char *to_string(const RequestBodyIngestionErrorCode code) noexcept
{
    switch (code)
    {
    case RequestBodyIngestionErrorCode::framing_failure:
        return "request body framing failed";
    case RequestBodyIngestionErrorCode::body_too_large:
        return "request body exceeds configured limit";
    case RequestBodyIngestionErrorCode::temporary_file_failure:
        return "request body temporary file failed";
    case RequestBodyIngestionErrorCode::cancelled:
        return "request body ingestion was cancelled";
    case RequestBodyIngestionErrorCode::deadline_exceeded:
        return "request body ingestion deadline expired";
    case RequestBodyIngestionErrorCode::resource_failure:
        return "request body ingestion exhausted storage resources";
    }
    return "unknown request body ingestion error";
}

} // namespace sparenode::http
