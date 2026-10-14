#pragma once

#include <functional>
#include <memory>
#include <span>

#include "sparenode/storage/temporary_file.hpp"

namespace sparenode::storage
{

/// @brief Creates one request-owned temporary file for bounded artifact ingestion.
using TemporaryFileFactory =
    std::function<Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>()>;

/// @brief Serializes temporary-file operations and isolates stalled native I/O.
///
/// Native writes and completion run directly when no cancellation or deadline policy is active.
/// Policy-bound operations use one reusable worker. If a request stops waiting, the worker retains
/// cleanup ownership until the native operation returns, while global accounting prevents more
/// than 32 active or orphaned operations from consuming process resources.
class TemporaryFileWriter final
{
  public:
    /// @brief Creates one owned artifact through an injected or native factory.
    /// @param[in] factory Optional controlled factory; empty selects `TemporaryFile::create()`.
    /// @return Writer with cleanup ownership, or a structured creation/allocation failure.
    [[nodiscard]] static Result<std::unique_ptr<TemporaryFileWriter>, TemporaryFileError>
    create(const TemporaryFileFactory &factory = {});

    TemporaryFileWriter(const TemporaryFileWriter &) = delete;
    TemporaryFileWriter &operator=(const TemporaryFileWriter &) = delete;
    TemporaryFileWriter(TemporaryFileWriter &&) = delete;
    TemporaryFileWriter &operator=(TemporaryFileWriter &&) = delete;

    /// @brief Releases idle resources or abandons stalled I/O with deferred cleanup ownership.
    ~TemporaryFileWriter();

    /// @brief Appends one bounded payload while respecting cancellation and deadline policy.
    /// @param[in] bytes Payload bytes copied before an isolated operation begins.
    /// @param[in] options Policy governing capacity acquisition and the native write.
    /// @return Success or a structured storage failure.
    [[nodiscard]] Result<void, TemporaryFileError>
    write(std::span<const std::byte> bytes, const TemporaryFileIoOptions &options = {});

    /// @brief Flushes and closes the artifact under the active I/O policy.
    /// @param[in] options Policy governing capacity acquisition, flush, and close.
    /// @return Success or a structured storage failure.
    [[nodiscard]] Result<void, TemporaryFileError>
    complete(const TemporaryFileIoOptions &options = {});

    /// @brief Shares the immutable completed artifact with its next consumer.
    /// @return Completed artifact, or null before successful completion.
    [[nodiscard]] std::shared_ptr<const TemporaryFile> artifact() const noexcept;

  private:
    struct FileWorker;

    /// @brief Takes cleanup ownership of one factory-created file.
    /// @param[in] file Exclusive artifact retained through direct or isolated operations.
    explicit TemporaryFileWriter(std::shared_ptr<TemporaryFile> file) noexcept;
    /// @brief Starts the reusable worker when the supplied policy can interrupt a caller.
    /// @param[in] options Policy whose presence selects isolated native I/O.
    /// @return Success, or a structured thread/allocation failure.
    [[nodiscard]] Result<void, TemporaryFileError>
    ensure_worker(const TemporaryFileIoOptions &options);

    std::shared_ptr<TemporaryFile> file_; ///< Owned until completion or deferred cleanup.
    std::unique_ptr<FileWorker> worker_;  ///< Optional isolated native-operation worker.
    bool completed_{};                    ///< True only after durable completion succeeds.
};

} // namespace sparenode::storage
