#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

#include "sparenode/filesystem/temporary_file.hpp"
#include "sparenode/http/request/http_body_decoder.hpp"

namespace sparenode::http
{

/// @brief Creates one request-owned temporary file or reports why creation failed.
using TemporaryBodyFileFactory = std::function<
    Result<std::shared_ptr<filesystem::TemporaryFile>, filesystem::TemporaryFileError>()>;

/// @brief Identifies why decoded request-body ingestion could not complete.
enum class RequestBodyIngestionErrorCode : std::uint8_t
{
    framing_failure,        ///< HTTP body framing or premature EOF was invalid.
    body_too_large,         ///< Decoded payload exceeded the configured transfer policy.
    temporary_file_failure, ///< Temporary creation, write, flush, or close failed.
    cancelled,              ///< The caller requested cancellation.
    resource_failure        ///< Artifact ownership allocation failed.
};

/// @brief Preserves protocol and filesystem detail for one ingestion failure.
struct RequestBodyIngestionError
{
    RequestBodyIngestionErrorCode code{};        ///< Stable ingestion failure category.
    HttpRequestParseError framing_error{};       ///< Populated for framing and size failures.
    filesystem::TemporaryFileError file_error{}; ///< Populated for temporary-file failures.
};

/// @brief Streams decoded fixed-length or chunked request bytes into temporary storage.
///
/// HTTP framing remains delegated to `HttpBodyDecoder`. The same file sink receives
/// payload from both framing modes, and owns partial-file cleanup on every failure.
class RequestBodyIngestor final
{
  public:
    /// @brief Initializes one ingestion stream from validated request metadata.
    /// @param[in] head Validated body framing selected by the HTTP parser.
    /// @param[in] limits Decoded payload and chunk metadata boundaries.
    /// @param[in] file_factory Optional alternate factory used by controlled environments.
    RequestBodyIngestor(const HttpRequestHead &head, const HttpRequestParserLimits &limits,
                        TemporaryBodyFileFactory file_factory = {});

    /// @brief Decodes fresh wire bytes and appends produced payload to temporary storage.
    /// @param[in] input Bytes beginning at the previous unconsumed body boundary.
    /// @param[in] stop_token Cancellation observed before decoding and filesystem writes.
    /// @return Decoder progress or a terminal structured ingestion failure.
    [[nodiscard]] Result<HttpBodyDecodeProgress, RequestBodyIngestionError>
    feed(std::span<const std::byte> input, const std::stop_token &stop_token = {});

    /// @brief Validates peer EOF and finalizes any completed nonempty artifact.
    /// @return Success only for an already complete body.
    [[nodiscard]] Result<void, RequestBodyIngestionError> finish();

    /// @brief Reports whether framing and temporary-file completion both succeeded.
    /// @return True only after the complete decoded body is safely finalized.
    [[nodiscard]] bool complete() const noexcept
    {
        return decoder_.complete() && finalized_ && !failure_.has_value();
    }

    /// @brief Returns the cumulative decoded payload size.
    /// @return Bytes successfully persisted to the temporary sink.
    [[nodiscard]] std::uint64_t body_size() const noexcept
    {
        return body_size_;
    }

    /// @brief Shares the completed nonempty artifact with the request lifetime.
    /// @return Artifact owner, or null for an empty or incomplete body.
    [[nodiscard]] std::shared_ptr<filesystem::TemporaryFile> artifact() const noexcept
    {
        return complete() ? artifact_ : nullptr;
    }

  private:
    /// @brief Converts one decoder failure without losing protocol detail.
    /// @param[in] error Terminal decoder failure.
    /// @return Matching ingestion category retaining the original parser error.
    [[nodiscard]] static RequestBodyIngestionError
    framing_failure(const HttpRequestParseError &error) noexcept;
    /// @brief Lazily creates the temporary sink for the first nonempty payload span.
    /// @return Success, or a structured creation/allocation failure.
    [[nodiscard]] Result<void, RequestBodyIngestionError> ensure_artifact();
    /// @brief Flushes and closes a nonempty sink after framing completes.
    /// @return Success, or a structured flush/close failure.
    [[nodiscard]] Result<void, RequestBodyIngestionError> finalize();
    /// @brief Appends one decoded span after observing cancellation.
    /// @param[in] output Decoded payload bytes to persist.
    /// @param[in] stop_token Cancellation checked before filesystem access.
    /// @return Success, cancellation, or a structured file failure.
    [[nodiscard]] Result<void, RequestBodyIngestionError>
    persist(std::span<const std::byte> output, const std::stop_token &stop_token);
    /// @brief Removes partial storage and preserves one terminal failure.
    /// @param[in] error Failure that terminated ingestion.
    /// @return The same failure for immediate propagation.
    [[nodiscard]] RequestBodyIngestionError fail(const RequestBodyIngestionError &error) noexcept;

    HttpBodyDecoder decoder_;               ///< Existing framing state shared by both body modes.
    TemporaryBodyFileFactory file_factory_; ///< Empty selects native private-file creation.
    std::shared_ptr<filesystem::TemporaryFile> artifact_; ///< Lazy nonempty file owner.
    std::uint64_t body_size_{};                           ///< Successfully persisted decoded bytes.
    bool finalized_{}; ///< Empty or file-backed body completed successfully.
    std::optional<RequestBodyIngestionError> failure_; ///< Preserves the terminal failure.
};

/// @brief Returns stable text for one request-body ingestion failure category.
[[nodiscard]] const char *to_string(RequestBodyIngestionErrorCode code) noexcept;

} // namespace sparenode::http
