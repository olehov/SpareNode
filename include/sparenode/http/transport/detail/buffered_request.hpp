#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "sparenode/http/request/detail/http_request_view_access.hpp"
#include "sparenode/http/transport/request_body_ingestor.hpp"

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

/// @brief Adapts streaming body ingestion to the storage policy selected from request metadata.
/// Metadata storage freezes after parsing; decoded bytes use bounded memory or temporary storage.
class BufferedRequest final
{
  public:
    /// @brief Retains the limits for one session-owned request.
    /// @param[in] limits Request metadata, decoded payload, and framing budgets.
    /// @param[in] file_factory Optional source of temporary body storage.
    /// @param[in] storage_policy_selector Optional route-specific body storage selector.
    explicit BufferedRequest(const HttpRequestParserLimits &limits,
                             storage::TemporaryFileFactory file_factory = {},
                             RequestBodyStoragePolicySelector storage_policy_selector = {})
        : limits_(limits), file_factory_(std::move(file_factory)),
          storage_policy_selector_(std::move(storage_policy_selector))
    {
    }

    /// @brief Securely erases retained request bytes before releasing their allocation.
    ~BufferedRequest();

    /// @brief Adds wire bytes, retaining metadata and streaming decoded payload to its sink.
    /// @param[in,out] input Fresh receive bytes, securely erased before this call returns.
    /// @param[in] options Cancellation and deadline observed by body filesystem operations.
    /// @return Success or a structured terminal parse/body error.
    [[nodiscard]] Result<void, BufferedRequestError>
    feed(std::span<std::byte> input, const storage::TemporaryFileIoOptions &options = {});

    /// @brief Resolves the body policy only after a complete request head is available.
    /// @param[in,out] input Fresh receive bytes, securely erased before this call returns.
    /// @param[in] options_provider Supplies the latest policy at the body boundary.
    /// @return Success or a structured parse/body error.
    [[nodiscard]] Result<void, BufferedRequestError> feed_with_body_options(
        std::span<std::byte> input,
        const std::function<storage::TemporaryFileIoOptions()> &options_provider);

    /// @brief Validates peer EOF and completes temporary-file cleanup on failure.
    /// @param[in] options Cancellation and deadline observed during finalization.
    /// @return Success only when the complete request boundary already arrived.
    [[nodiscard]] Result<void, BufferedRequestError>
    finish(const storage::TemporaryFileIoOptions &options = {});

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
    /// @param[in] options Cancellation and deadline retained for later body publication.
    /// @return Complete request; callers must first check complete().
    [[nodiscard]] HttpRequestView request(const storage::TemporaryFileIoOptions &options = {}) const
    {
        if (!head_.has_value() || !ingestor_.has_value() || !ingestor_->complete())
        {
            throw std::logic_error("HTTP request is incomplete");
        }
        const auto &ingestor = ingestor_.value();
        if (const auto memory = ingestor.memory_body())
        {
            return HttpRequestViewAccess::create(head_.value(), *memory, memory);
        }
        return HttpRequestViewAccess::create_file_backed(head_.value(), ingestor.body_size(),
                                                         ingestor.artifact(), options);
    }

    /// @brief Consumes the header expectation decision once, before waiting for more body bytes.
    /// @return Continue for an incomplete expected body, unsupported for unknown expectations.
    [[nodiscard]] RequestExpectation take_expectation();

  private:
    /// @brief Securely erases and logically releases all accumulated metadata bytes.
    void erase_metadata() noexcept;

    /// @brief Drains decoder output into the request's temporary-file sink.
    /// @param[in] input New body wire bytes, excluding previously consumed data.
    /// @param[in] options Cancellation and deadline observed by body filesystem operations.
    /// @return Success or terminal body syntax/limit failure.
    [[nodiscard]] Result<void, BufferedRequestError>
    feed_body(std::span<const std::byte> input, const storage::TemporaryFileIoOptions &options);

    bool expectation_checked_{};                 ///< Prevents repeated interim responses.
    HttpRequestParserLimits limits_;             ///< Session parser and decoder limits.
    storage::TemporaryFileFactory file_factory_; ///< Optional controlled body-file factory.
    RequestBodyStoragePolicySelector
        storage_policy_selector_;                 ///< Optional route-specific body storage choice.
    std::vector<std::byte> metadata_;             ///< Frozen metadata backing all header views.
    std::optional<HttpRequestHead> head_;         ///< Validated borrowed request metadata.
    std::optional<RequestBodyIngestor> ingestor_; ///< Framing and temporary-file body sink.
};
} // namespace sparenode::http::detail
