#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

#include "sparenode/http/request/http_body_decoder.hpp"
#include "sparenode/http/request/request_body_storage_policy.hpp"
#include "sparenode/storage/temporary_file_writer.hpp"

namespace sparenode::http
{

/// @brief Identifies why decoded request-body ingestion could not complete.
enum class RequestBodyIngestionErrorCode : std::uint8_t
{
    framing_failure,        ///< HTTP body framing or premature EOF was invalid.
    body_too_large,         ///< Decoded payload exceeded the configured transfer policy.
    temporary_file_failure, ///< Temporary creation, write, flush, or close failed.
    cancelled,              ///< The caller requested cancellation.
    deadline_exceeded,      ///< The active request receive deadline expired.
    resource_failure        ///< Artifact ownership or bounded file-operation capacity failed.
};

/// @brief Preserves protocol and temporary-storage detail for one ingestion failure.
struct RequestBodyIngestionError
{
    RequestBodyIngestionErrorCode code{};     ///< Stable ingestion failure category.
    HttpRequestParseError framing_error{};    ///< Populated for framing and size failures.
    storage::TemporaryFileError file_error{}; ///< Populated for temporary-file failures.
};

/// @brief Streams decoded fixed-length or chunked request bytes into temporary storage.
///
/// HTTP framing remains delegated to `HttpBodyDecoder`. The same file sink receives
/// payload from both framing modes, and owns partial-file cleanup on every failure.
/// A stalled native file operation retains its file owner and fixed payload buffer
/// until it returns; the request worker can stop waiting at cancellation or deadline.
class RequestBodyIngestor final
{
  public:
    /// @brief Initializes one ingestion stream from validated request metadata.
    /// @param[in] head Validated body framing selected by the HTTP parser.
    /// @param[in] limits Decoded payload and chunk metadata boundaries.
    /// @param[in] file_factory Injected storage factory; empty selects native temporary storage.
    /// @param[in] storage_mode Destination used for decoded payload bytes.
    RequestBodyIngestor(
        const HttpRequestHead &head, const HttpRequestParserLimits &limits,
        storage::TemporaryFileFactory file_factory = {},
        RequestBodyStorageMode storage_mode = RequestBodyStorageMode::temporary_file);

    /// @brief Stops an idle I/O worker or leaves a stalled one owning its artifact.
    ~RequestBodyIngestor();

    /// @brief Decodes fresh wire bytes and appends produced payload to temporary storage.
    /// @param[in] input Bytes beginning at the previous unconsumed body boundary.
    /// @param[in] options Cancellation and deadline propagated through storage writes.
    /// @return Decoder progress or a terminal structured ingestion failure.
    [[nodiscard]] Result<HttpBodyDecodeProgress, RequestBodyIngestionError>
    feed(std::span<const std::byte> input, const storage::TemporaryFileIoOptions &options = {});

    /// @brief Validates peer EOF and finalizes any completed nonempty artifact.
    /// @param[in] options Cancellation and deadline propagated through finalization.
    /// @return Success only for an already complete body.
    [[nodiscard]] Result<void, RequestBodyIngestionError>
    finish(const storage::TemporaryFileIoOptions &options = {});

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
    [[nodiscard]] std::shared_ptr<const storage::TemporaryFile> artifact() const noexcept
    {
        return complete() ? artifact_ : nullptr;
    }

    /// @brief Shares a completed payload retained only in memory.
    /// @return Immutable body owner, or null for file-backed and incomplete bodies.
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> memory_body() const noexcept
    {
        return complete() && storage_mode_ == RequestBodyStorageMode::memory ? memory_body_
                                                                             : nullptr;
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
    /// @param[in] options Cancellation and deadline applied to finalization.
    /// @return Success, or a structured flush/close failure.
    [[nodiscard]] Result<void, RequestBodyIngestionError>
    finalize(const storage::TemporaryFileIoOptions &options);
    /// @brief Appends one decoded span after observing cancellation.
    /// @param[in] output Decoded payload bytes to persist.
    /// @param[in] options Cancellation and deadline applied to storage access.
    /// @return Success, cancellation, or a structured file failure.
    [[nodiscard]] Result<void, RequestBodyIngestionError>
    persist(std::span<const std::byte> output, const storage::TemporaryFileIoOptions &options);
    /// @brief Maps one temporary-file failure into the ingestion contract.
    /// @param[in] error Structured temporary-storage failure.
    /// @return Cancellation, deadline, or general temporary-file failure.
    [[nodiscard]] static RequestBodyIngestionError
    file_failure(const storage::TemporaryFileError &error) noexcept;
    /// @brief Removes partial storage and preserves one terminal failure.
    /// @param[in] error Failure that terminated ingestion.
    /// @return The same failure for immediate propagation.
    [[nodiscard]] RequestBodyIngestionError fail(const RequestBodyIngestionError &error) noexcept;

    HttpBodyDecoder decoder_;             ///< Existing framing state shared by both body modes.
    RequestBodyStorageMode storage_mode_; ///< Selected lifetime for decoded payload bytes.
    storage::TemporaryFileFactory file_factory_;          ///< Injected temporary-storage factory.
    std::shared_ptr<std::vector<std::byte>> memory_body_; ///< Optional sensitive memory payload.
    std::shared_ptr<const storage::TemporaryFile> artifact_; ///< Completed nonempty artifact.
    std::unique_ptr<storage::TemporaryFileWriter> writer_;   ///< Bounded isolated storage writer.
    std::uint64_t body_size_{}; ///< Successfully persisted decoded bytes.
    bool finalized_{};          ///< Empty or file-backed body completed successfully.
    std::optional<RequestBodyIngestionError> failure_; ///< Preserves the terminal failure.
};

/// @brief Returns stable text for one request-body ingestion failure category.
[[nodiscard]] const char *to_string(RequestBodyIngestionErrorCode code) noexcept;

} // namespace sparenode::http
