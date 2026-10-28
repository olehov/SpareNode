#include "sparenode/http/transport/request_body_ingestor.hpp"

#include <array>
#include <chrono>
#include <utility>

#include "sparenode/foundation/concurrency/interruption.hpp"
#include "sparenode/foundation/memory/secure_erase.hpp"
#include "sparenode/http/request/detail/body_decode_buffer.hpp"

namespace sparenode::http
{
namespace
{

/// @brief Erases a sensitive in-memory request body before releasing its allocation.
void delete_sensitive_body(std::vector<std::byte> *body) noexcept
{
    if (body != nullptr)
    {
        memory::secure_erase(*body);
        delete body;
    }
}

/// @brief Converts active policy state into a terminal ingestion error when interrupted.
[[nodiscard]] std::optional<RequestBodyIngestionError>
interruption(const storage::TemporaryFileIoOptions &options) noexcept
{
    switch (concurrency::interruption_status(options.stop_token, options.deadline))
    {
    case concurrency::InterruptionStatus::cancelled:
        return RequestBodyIngestionError{RequestBodyIngestionErrorCode::cancelled, {}, {}};
    case concurrency::InterruptionStatus::deadline_exceeded:
        return RequestBodyIngestionError{RequestBodyIngestionErrorCode::deadline_exceeded, {}, {}};
    case concurrency::InterruptionStatus::none:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

/// @brief Initializes framing and retains the injected temporary-storage factory.
RequestBodyIngestor::RequestBodyIngestor(const HttpRequestHead &head,
                                         const HttpRequestParserLimits &limits,
                                         storage::TemporaryFileFactory file_factory,
                                         const RequestBodyStorageMode storage_mode)
    : decoder_(head, limits), storage_mode_(storage_mode), file_factory_(std::move(file_factory))
{
    if (storage_mode_ == RequestBodyStorageMode::memory)
    {
        memory_body_ = std::shared_ptr<std::vector<std::byte>>(new std::vector<std::byte>,
                                                               delete_sensitive_body);
        memory_body_->reserve(limits.max_body_bytes);
    }
    finalized_ = decoder_.complete();
}

RequestBodyIngestor::~RequestBodyIngestor() = default;

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

/// @brief Maps storage policy interruption separately from native failures.
RequestBodyIngestionError
RequestBodyIngestor::file_failure(const storage::TemporaryFileError &error) noexcept
{
    switch (error.code)
    {
    case storage::TemporaryFileErrorCode::cancelled:
        return {RequestBodyIngestionErrorCode::cancelled, {}, error};
    case storage::TemporaryFileErrorCode::deadline_exceeded:
        return {RequestBodyIngestionErrorCode::deadline_exceeded, {}, error};
    case storage::TemporaryFileErrorCode::resource_allocation_failed:
        return {RequestBodyIngestionErrorCode::resource_failure, {}, error};
    default:
        return {RequestBodyIngestionErrorCode::temporary_file_failure, {}, error};
    }
}

/// @brief Releases partial storage before preserving the terminal ingestion failure.
RequestBodyIngestionError RequestBodyIngestor::fail(const RequestBodyIngestionError &error) noexcept
{
    writer_.reset();
    artifact_.reset();
    failure_ = error;
    return error;
}

/// @brief Lazily creates bounded temporary storage for the first decoded payload bytes.
Result<void, RequestBodyIngestionError> RequestBodyIngestor::ensure_artifact()
{
    if (writer_)
    {
        return {};
    }
    auto writer = storage::TemporaryFileWriter::create(file_factory_);
    if (!writer)
    {
        return unexpected(fail(file_failure(writer.error())));
    }
    writer_ = std::move(writer).value();
    return {};
}

/// @brief Flushes and closes the completed nonempty artifact under active I/O policy.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::finalize(const storage::TemporaryFileIoOptions &options)
{
    if (finalized_)
    {
        return {};
    }
    if (writer_)
    {
        if (auto completed = writer_->complete(options); !completed)
        {
            return unexpected(fail(file_failure(completed.error())));
        }
        artifact_ = writer_->artifact();
        if (!artifact_)
        {
            return unexpected(fail(RequestBodyIngestionError{
                RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
        }
        writer_.reset();
    }
    finalized_ = true;
    return {};
}

/// @brief Persists one decoded span under active cancellation and deadline policy.
Result<void, RequestBodyIngestionError>
RequestBodyIngestor::persist(const std::span<const std::byte> output,
                             const storage::TemporaryFileIoOptions &options)
{
    if (const auto interrupted = interruption(options); interrupted.has_value())
    {
        return unexpected(fail(interrupted.value()));
    }
    if (storage_mode_ == RequestBodyStorageMode::memory)
    {
        try
        {
            memory_body_->insert(memory_body_->end(), output.begin(), output.end());
            body_size_ += output.size();
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return unexpected(fail(RequestBodyIngestionError{
                RequestBodyIngestionErrorCode::resource_failure, {}, {}}));
        }
    }
    if (auto created = ensure_artifact(); !created)
    {
        return unexpected(created.error());
    }
    if (auto written = writer_->write(output, options); !written)
    {
        return unexpected(fail(file_failure(written.error())));
    }
    body_size_ += output.size();
    return {};
}

/// @brief Decodes fresh wire bytes and persists only emitted payload spans.
Result<HttpBodyDecodeProgress, RequestBodyIngestionError>
RequestBodyIngestor::feed(std::span<const std::byte> input,
                          const storage::TemporaryFileIoOptions &options)
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
RequestBodyIngestor::finish(const storage::TemporaryFileIoOptions &options)
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
