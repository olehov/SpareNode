#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

#include "sparenode/http/request/detail/http_request_view_access.hpp"
#include "sparenode/http/request/request_body_ingestor.hpp"

namespace sparenode::http::detail
{
/// @brief One-time action selected after validated request headers arrive.
enum class RequestExpectation : std::uint8_t
{
    none,             ///< No interim or final expectation action is required.
    continue_request, ///< A supported expectation awaits additional body bytes.
    unsupported       ///< At least one expectation cannot be met.
};

/// @brief Distinguishes metadata parsing from body ingestion failures.
using BufferedRequestError = std::variant<HttpRequestParseError, RequestBodyIngestionError>;

/// @brief Adapts streaming body ingestion to the session's file-backed route contract.
/// Metadata storage freezes after parsing; decoded body bytes stream to temporary storage.
class BufferedRequest final
{
  public:
    /// @brief Retains the limits for one session-owned request.
    /// @param[in] limits Request metadata, decoded payload, and framing budgets.
    /// @param[in] file_factory Optional source of temporary body storage.
    explicit BufferedRequest(const HttpRequestParserLimits &limits,
                             TemporaryBodyFileFactory file_factory = {})
        : limits_(limits), file_factory_(std::move(file_factory))
    {
    }

    /// @brief Adds wire bytes, retaining metadata and streaming decoded payload to disk.
    /// @param[in] input Fresh receive bytes; trailing pipelined bytes are ignored after completion.
    /// @param[in] options Cancellation and deadline observed by body filesystem operations.
    /// @return Success or a structured terminal parse/body error.
    [[nodiscard]] Result<void, BufferedRequestError>
    feed(std::span<const std::byte> input, const filesystem::TemporaryFileIoOptions &options = {});

    /// @brief Validates peer EOF and completes temporary-file cleanup on failure.
    /// @param[in] options Cancellation and deadline observed during finalization.
    /// @return Success only when the complete request boundary already arrived.
    [[nodiscard]] Result<void, BufferedRequestError>
    finish(const filesystem::TemporaryFileIoOptions &options = {});

    /// @brief Reports the transition to body ingestion for inactivity accounting.
    /// @return True once a valid complete request head is retained.
    [[nodiscard]] bool reading_body() const noexcept
    {
        return head_.has_value();
    }
    /// @brief Reports that body framing, including trailers, has completed.
    /// @return True when the body decoder reaches its final boundary without failure.
    [[nodiscard]] bool complete() const noexcept
    {
        return ingestor_.has_value() && ingestor_->complete();
    }
    /// @brief Reports whether the peer has sent no request bytes at all.
    /// @return True before receiving any request metadata.
    [[nodiscard]] bool empty() const noexcept
    {
        return metadata_.empty();
    }
    /// @brief Builds a request borrowing this adapter's stable storage after completion.
    /// @return Complete request; callers must first check complete().
    [[nodiscard]] HttpRequestView request() const
    {
        if (!head_.has_value() || !ingestor_.has_value() || !ingestor_->complete())
        {
            throw std::logic_error("HTTP request is incomplete");
        }
        const auto &ingestor = ingestor_.value();
        return HttpRequestViewAccess::create_file_backed(head_.value(), ingestor.body_size(),
                                                         ingestor.artifact());
    }

    /// @brief Consumes the header expectation decision once, before waiting for more body bytes.
    /// @return Continue for an incomplete expected body, unsupported for unknown expectations.
    [[nodiscard]] RequestExpectation take_expectation();

  private:
    /// @brief Drains decoder output into the request's temporary-file sink.
    /// @param[in] input New body wire bytes, excluding previously consumed data.
    /// @param[in] options Cancellation and deadline observed by body filesystem operations.
    /// @return Success or terminal body syntax/limit failure.
    [[nodiscard]] Result<void, BufferedRequestError>
    feed_body(std::span<const std::byte> input, const filesystem::TemporaryFileIoOptions &options);

    bool expectation_checked_{};                  ///< Prevents repeated interim responses.
    HttpRequestParserLimits limits_;              ///< Session parser and decoder limits.
    TemporaryBodyFileFactory file_factory_;       ///< Optional controlled body-file factory.
    std::vector<std::byte> metadata_;             ///< Frozen metadata backing all header views.
    std::optional<HttpRequestHead> head_;         ///< Validated borrowed request metadata.
    std::optional<RequestBodyIngestor> ingestor_; ///< Framing and temporary-file body sink.
};
} // namespace sparenode::http::detail
