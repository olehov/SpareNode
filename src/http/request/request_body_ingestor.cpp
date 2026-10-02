#include "sparenode/http/request/request_body_ingestor.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sparenode/http/request/detail/body_decode_buffer.hpp"

namespace sparenode::http
{
namespace
{

using FileOperationResult = Result<void, filesystem::TemporaryFileError>;

/// @brief Caps stalled workers at 32 and their 16-KiB payload copies at 512 KiB total.
constexpr std::size_t maximum_active_file_operations = 32;
std::atomic_size_t active_file_operations{};

/// @brief Keeps the result alive after the HTTP worker stops waiting for native I/O.
struct FileOperationState
{
    std::mutex mutex;                          ///< Protects the result handoff.
    std::condition_variable_any completed;     ///< Wakes the HTTP worker on completion or stop.
    std::optional<FileOperationResult> result; ///< Written once by the I/O worker.
};

/// @brief Reserves one of the fixed number of concurrently active native operations.
[[nodiscard]] bool reserve_file_operation() noexcept
{
    auto count = active_file_operations.load(std::memory_order_relaxed);
    while (count < maximum_active_file_operations)
    {
        if (active_file_operations.compare_exchange_weak(count, count + 1,
                                                         std::memory_order_acq_rel))
        {
            return true;
        }
    }
    return false;
}

/// @brief Converts worker creation failures into the existing resource error category.
[[nodiscard]] FileOperationResult file_worker_unavailable(const int native_code = 0)
{
    return unexpected(filesystem::TemporaryFileError{
        filesystem::TemporaryFileErrorCode::resource_allocation_failed, native_code});
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

/// @brief Publishes one worker result and relinquishes its bounded concurrency slot.
void perform_file_operation(const std::shared_ptr<FileOperationState> &state,
                            std::function<FileOperationResult()> operation) noexcept
{
    FileOperationResult result;
    try
    {
        result = operation();
    }
    catch (...)
    {
        result = file_worker_unavailable();
    }
    // Release the captured file and payload here so cleanup remains inside the bounded slot.
    operation = {};
    {
        std::lock_guard lock(state->mutex);
        state->result.emplace(result);
    }
    active_file_operations.fetch_sub(1, std::memory_order_acq_rel);
    state->completed.notify_all();
}

/// @brief Waits no longer than the request policy for a separately owned file operation.
[[nodiscard]] FileOperationResult
await_file_operation(std::thread worker, const std::shared_ptr<FileOperationState> &state,
                     const filesystem::TemporaryFileIoOptions &options)
{
    std::unique_lock lock(state->mutex);
    const auto ready = [&state] { return state->result.has_value(); };
    if (options.deadline.has_value())
    {
        static_cast<void>(
            state->completed.wait_until(lock, options.stop_token, options.deadline.value(), ready));
    }
    else
    {
        static_cast<void>(state->completed.wait(lock, options.stop_token, ready));
    }
    if (state->result.has_value())
    {
        const auto result = state->result.value_or(file_worker_unavailable());
        lock.unlock();
        worker.join();
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
        return result;
    }
    lock.unlock();
    try
    {
        worker.detach();
    }
    catch (const std::system_error &)
    {
        worker.join(); // Preserve ownership if the OS refuses to detach the worker.
    }
    return unexpected(filesystem::TemporaryFileError{
        options.stop_token.stop_requested() ? filesystem::TemporaryFileErrorCode::cancelled
                                            : filesystem::TemporaryFileErrorCode::deadline_exceeded,
        0});
}

/// @brief Isolates a native operation only when the caller supplies an interruption policy.
[[nodiscard]] FileOperationResult
run_file_operation(std::function<FileOperationResult()> operation,
                   const filesystem::TemporaryFileIoOptions &options)
{
    if (!options.deadline.has_value() && !options.stop_token.stop_possible())
    {
        return operation();
    }
    if (!reserve_file_operation())
    {
        return file_worker_unavailable();
    }
    std::shared_ptr<FileOperationState> state;
    try
    {
        state = std::make_shared<FileOperationState>();
    }
    catch (const std::bad_alloc &)
    {
        active_file_operations.fetch_sub(1, std::memory_order_acq_rel);
        return file_worker_unavailable();
    }
    std::thread worker;
    try
    {
        worker = std::thread([state, operation = std::move(operation)]() mutable
                             { perform_file_operation(state, std::move(operation)); });
    }
    catch (const std::system_error &error)
    {
        active_file_operations.fetch_sub(1, std::memory_order_acq_rel);
        return file_worker_unavailable(error.code().value());
    }
    catch (const std::bad_alloc &)
    {
        active_file_operations.fetch_sub(1, std::memory_order_acq_rel);
        return file_worker_unavailable();
    }
    return await_file_operation(std::move(worker), state, options);
}

} // namespace

/// @brief Initializes framing and retains an optional temporary-file factory.
RequestBodyIngestor::RequestBodyIngestor(const HttpRequestHead &head,
                                         const HttpRequestParserLimits &limits,
                                         TemporaryBodyFileFactory file_factory)
    : decoder_(head, limits), file_factory_(std::move(file_factory))
{
    finalized_ = decoder_.complete();
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
    artifact_.reset();
    failure_ = error;
    return error;
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
        FileOperationResult completed;
        try
        {
            const auto owned_file = artifact_;
            completed = run_file_operation([owned_file, options]
                                           { return owned_file->complete(options); }, options);
        }
        catch (const std::bad_alloc &)
        {
            return unexpected(fail(RequestBodyIngestionError{
                RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
        }
        if (!completed)
        {
            return unexpected(fail(file_failure(completed.error())));
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
    FileOperationResult written;
    try
    {
        const auto owned_file = artifact_;
        std::vector<std::byte> payload(output.begin(), output.end());
        written = run_file_operation([owned_file, payload = std::move(payload), options]
                                     { return owned_file->write(payload, options); }, options);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(fail(
            RequestBodyIngestionError{RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
    }
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
        return "request body ingestion exhausted memory";
    }
    return "unknown request body ingestion error";
}

} // namespace sparenode::http
