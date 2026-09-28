#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies why a shared file could not be opened or read safely.
enum class FileReadErrorCode : std::uint8_t
{
    invalid_path,               ///< The untrusted request failed SafePath validation.
    not_found,                  ///< The requested filesystem object does not exist.
    not_regular_file,           ///< The requested object is not a regular file.
    permission_denied,          ///< The host denied access to the file.
    outside_shared_root,        ///< The opened object did not remain inside the shared root.
    filesystem_failure,         ///< A host filesystem operation failed unexpectedly.
    cancelled,                  ///< The caller requested cancellation before another read.
    resource_allocation_failed, ///< Stream-owned memory could not be allocated.
};

/// @brief Preserves portable and subsystem-specific file-read failure details.
struct FileReadError
{
    FileReadErrorCode code{};                    ///< Stable file-read failure category.
    std::error_code system_error{};              ///< Optional native filesystem detail.
    std::optional<SafePathErrorCode> path_error; ///< Optional SafePath rejection category.
};

/// @brief Owns a stable, root-confined native file handle for incremental reads.
class FileReadStream final
{
  public:
    FileReadStream(const FileReadStream &) = delete;
    FileReadStream &operator=(const FileReadStream &) = delete;

    /// @brief Transfers ownership of the stable native file handle.
    FileReadStream(FileReadStream &&) noexcept;
    /// @brief Replaces this stream with another stable native file handle.
    /// @return This stream after taking ownership.
    FileReadStream &operator=(FileReadStream &&) noexcept;
    /// @brief Closes the owned native file handle.
    ~FileReadStream();

    /// @brief Resolves and opens one untrusted path inside a shared root.
    /// @param[in] shared_root Validated root that must contain the opened file.
    /// @param[in] requested_path URL-encoded relative UTF-8 file path.
    /// @return A stable stream or a structured safe failure.
    [[nodiscard]] static Result<FileReadStream, FileReadError>
    open(const configuration::SharedRoot &shared_root, std::string_view requested_path);

    /// @brief Returns the file length captured from the opened native handle.
    /// @return Exact byte length used for HTTP Content-Length framing.
    [[nodiscard]] std::uint64_t size() const noexcept;

    /// @brief Reads the next caller-bounded file chunk.
    /// @param[out] destination Storage that bounds the native read operation.
    /// @param[in] stop_token Cancellation observed before starting another native read.
    /// @return Bytes produced, zero at end of file, or a structured failure.
    [[nodiscard]] Result<std::size_t, FileReadError> read(std::span<std::byte> destination,
                                                          const std::stop_token &stop_token);

  private:
    struct Implementation;

    /// @brief Takes ownership of a platform-specific opened file.
    /// @param[in] implementation Stable native handle and captured metadata.
    explicit FileReadStream(std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_; ///< Platform-specific handle and file size.
};

/// @brief Returns stable text for one file-read failure category.
[[nodiscard]] const char *to_string(FileReadErrorCode code) noexcept;

} // namespace sparenode::filesystem
