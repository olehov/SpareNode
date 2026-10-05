#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>

#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies one failure while owning or writing a private temporary file.
enum class TemporaryFileErrorCode : std::uint8_t
{
    temporary_directory_unavailable, ///< The platform temporary directory could not be resolved.
    create_failed,                   ///< No collision-free private file could be created.
    write_failed,                    ///< The native file write did not commit all supplied bytes.
    flush_failed,                    ///< Durable completion of file contents failed.
    close_failed,                    ///< Closing the completed native file failed.
    cancelled,                       ///< The caller cancelled the active file operation.
    deadline_exceeded,               ///< The active file-operation deadline expired.
    invalid_state,                   ///< The operation is incompatible with the file lifecycle.
    resource_allocation_failed       ///< Path or name construction exhausted memory.
};

/// @brief Carries cooperative cancellation and an absolute file-operation deadline.
struct TemporaryFileIoOptions
{
    std::stop_token stop_token{}; ///< Token observed around bounded native operations.
    std::optional<std::chrono::steady_clock::time_point> deadline{}; ///< Optional expiry.
};

/// @brief Preserves a portable category and optional platform error value.
struct TemporaryFileError
{
    TemporaryFileErrorCode code{}; ///< Stable failure category.
    int native_code{};             ///< `errno` or Win32 error, otherwise zero.
};

/// @brief Owns a uniquely created private temporary file until explicit release.
///
/// Destruction closes the native handle and removes the path. A completed file is
/// immutable through this API and may be transferred later to upload finalization.
class TemporaryFile
{
  public:
    /// @brief Creates a private file without replacing an existing directory entry.
    /// @return Writable owner or a structured creation failure.
    [[nodiscard]] static Result<TemporaryFile, TemporaryFileError> create();

    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    /// @brief Transfers native handle and cleanup ownership from another file.
    /// @param[in,out] other Owner left without a handle or cleanup path.
    TemporaryFile(TemporaryFile &&other) noexcept;

    /// @brief Cleans the current artifact and takes another file's ownership.
    /// @param[in,out] other Owner left without a handle or cleanup path.
    /// @return This owner after the transfer.
    TemporaryFile &operator=(TemporaryFile &&other) noexcept;

    /// @brief Closes and removes the file unless ownership was released.
    virtual ~TemporaryFile();

    /// @brief Appends every supplied byte while the file remains writable.
    /// @param[in] bytes Decoded request body bytes to append.
    /// @param[in] options Cancellation and deadline observed around bounded native writes.
    /// @return Success or a structured native write failure.
    [[nodiscard]] virtual Result<void, TemporaryFileError>
    write(std::span<const std::byte> bytes, const TemporaryFileIoOptions &options = {});

    /// @brief Flushes and closes the file before publishing it as a completed artifact.
    /// @param[in] options Cancellation and deadline observed around flush and close.
    /// @return Success or a structured flush/close failure.
    [[nodiscard]] virtual Result<void, TemporaryFileError>
    complete(const TemporaryFileIoOptions &options = {});

    /// @brief Returns the owned path while the artifact remains unreleased.
    /// @return Filesystem path removed by destruction until release succeeds.
    [[nodiscard]] virtual const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

    /// @brief Returns the exact number of successfully written bytes.
    /// @return Cumulative byte count accepted by write().
    [[nodiscard]] virtual std::uint64_t size() const noexcept
    {
        return size_;
    }

    /// @brief Reports whether flushing and native close completed successfully.
    /// @return True only after complete() succeeds.
    [[nodiscard]] virtual bool completed() const noexcept
    {
        return completed_;
    }

    /// @brief Relinquishes automatic path removal after successful completion.
    /// @return Former owned path or an invalid-state error.
    [[nodiscard]] virtual Result<std::filesystem::path, TemporaryFileError> release();

  private:
    /// @brief Adopts one exclusive native file and its cleanup path.
    /// @param[in] path Filesystem entry removed while ownership remains local.
    /// @param[in] native_handle Platform handle encoded as an integer.
    TemporaryFile(std::filesystem::path path, std::intptr_t native_handle) noexcept;
    /// @brief Closes any open handle and removes an owned path without throwing.
    void cleanup() noexcept;

  protected:
    /// @brief Creates an inert base for controlled alternate implementations.
    ///
    /// Production callers use `create()`. This constructor permits an injected
    /// implementation to report portable filesystem failures without native I/O.
    TemporaryFile() noexcept = default;

  private:
    std::filesystem::path path_;      ///< Entry removed unless released.
    std::intptr_t native_handle_{-1}; ///< Opaque writable handle, or `-1` after close.
    std::uint64_t size_{};            ///< Bytes committed through write().
    bool completed_{};                ///< Flush and close completed successfully.
};

/// @brief Returns stable text for one temporary-file failure category.
[[nodiscard]] const char *to_string(TemporaryFileErrorCode code) noexcept;

} // namespace sparenode::filesystem
