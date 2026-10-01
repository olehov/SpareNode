#include "sparenode/http/request/request_body_ingestor.hpp"

#include <array>
#include <new>
#include <utility>

#include "sparenode/http/request/detail/body_decode_buffer.hpp"

namespace sparenode::http
{

RequestBodyIngestor::RequestBodyIngestor(const HttpRequestHead &head,
                                         const HttpRequestParserLimits &limits,
                                         TemporaryBodyFileFactory file_factory)
    : decoder_(head, limits), file_factory_(std::move(file_factory))
{
    finalized_ = decoder_.complete();
}

RequestBodyIngestionError
RequestBodyIngestor::framing_failure(const HttpRequestParseError &error) noexcept
{
    return {error.code == HttpRequestParseErrorCode::body_too_large
                ? RequestBodyIngestionErrorCode::body_too_large
                : RequestBodyIngestionErrorCode::framing_failure,
            error,
            {}};
}

RequestBodyIngestionError RequestBodyIngestor::fail(const RequestBodyIngestionError &error) noexcept
{
    artifact_.reset();
    failure_ = error;
    return error;
}

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
                return unexpected(fail(RequestBodyIngestionError{
                    RequestBodyIngestionErrorCode::temporary_file_failure, {}, file.error()}));
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
            return unexpected(fail(RequestBodyIngestionError{
                RequestBodyIngestionErrorCode::temporary_file_failure, {}, file.error()}));
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

Result<void, RequestBodyIngestionError> RequestBodyIngestor::finalize()
{
    if (finalized_)
    {
        return {};
    }
    if (artifact_)
    {
        if (const auto completed = artifact_->complete(); !completed)
        {
            return unexpected(fail(RequestBodyIngestionError{
                RequestBodyIngestionErrorCode::temporary_file_failure, {}, completed.error()}));
        }
    }
    finalized_ = true;
    return {};
}

Result<void, RequestBodyIngestionError>
RequestBodyIngestor::persist(const std::span<const std::byte> output,
                             const std::stop_token &stop_token)
{
    if (stop_token.stop_requested())
    {
        return unexpected(
            fail(RequestBodyIngestionError{RequestBodyIngestionErrorCode::cancelled, {}, {}}));
    }
    if (auto created = ensure_artifact(); !created)
    {
        return unexpected(created.error());
    }
    if (auto written = artifact_->write(output); !written)
    {
        return unexpected(fail(RequestBodyIngestionError{
            RequestBodyIngestionErrorCode::temporary_file_failure, {}, written.error()}));
    }
    body_size_ += output.size();
    return {};
}

Result<HttpBodyDecodeProgress, RequestBodyIngestionError>
RequestBodyIngestor::feed(std::span<const std::byte> input, const std::stop_token &stop_token)
{
    if (failure_)
    {
        return unexpected(failure_.value());
    }
    if (stop_token.stop_requested())
    {
        return unexpected(
            fail(RequestBodyIngestionError{RequestBodyIngestionErrorCode::cancelled, {}, {}}));
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
            if (auto persisted = persist(std::span(output).first(progress->produced), stop_token);
                !persisted)
            {
                return unexpected(persisted.error());
            }
        }
        if (progress->complete)
        {
            if (auto completed = finalize(); !completed)
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

Result<void, RequestBodyIngestionError> RequestBodyIngestor::finish()
{
    if (failure_)
    {
        return unexpected(failure_.value());
    }
    if (const auto finished = decoder_.finish(); !finished)
    {
        return unexpected(fail(framing_failure(finished.error())));
    }
    return finalize();
}

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
    case RequestBodyIngestionErrorCode::resource_failure:
        return "request body ingestion exhausted memory";
    }
    return "unknown request body ingestion error";
}

} // namespace sparenode::http
