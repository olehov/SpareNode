#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "sparenode/application/filesystem_resource_coordinator.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/filesystem/upload_finalizer.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/storage/temporary_file.hpp"

namespace sparenode::application
{

/// @brief Identifies a transport-neutral filesystem read failure.
enum class FilesystemReadErrorCode : std::uint8_t
{
    forbidden,                  ///< The location policy rejects read operations.
    coordination_failed,        ///< Coordinated resource ownership was not granted.
    directory_listing_failed,   ///< Directory listing failed below the application boundary.
    file_read_failed,           ///< File opening failed below the application boundary.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a filesystem read failure.
using FilesystemReadErrorDetail =
    std::variant<std::monostate, FilesystemResourceAccessError, filesystem::DirectoryListingError,
                 filesystem::FileReadError>;

/// @brief Preserves a stable read category and optional lower-level failure detail.
struct FilesystemReadError
{
    FilesystemReadErrorCode code{};     ///< Stable application failure category.
    FilesystemReadErrorDetail detail{}; ///< Matching lower-level error when applicable.
};

/// @brief Identifies a transport-neutral upload failure.
enum class FilesystemUploadErrorCode : std::uint8_t
{
    forbidden,                  ///< The location policy rejects write operations.
    invalid_upload,             ///< Upload content is incomplete or inconsistent.
    coordination_failed,        ///< Coordinated resource ownership was not granted.
    temporary_file_failed,      ///< Upload materialization failed in temporary storage.
    upload_finalization_failed, ///< Atomic upload publication failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with an upload failure.
using FilesystemUploadErrorDetail =
    std::variant<std::monostate, FilesystemResourceAccessError, storage::TemporaryFileError,
                 filesystem::UploadFinalizationError>;

/// @brief Preserves a stable upload category and optional lower-level failure detail.
struct FilesystemUploadError
{
    FilesystemUploadErrorCode code{};     ///< Stable application failure category.
    FilesystemUploadErrorDetail detail{}; ///< Matching lower-level error when applicable.
};

/// @brief Identifies a transport-neutral directory-creation failure.
enum class FilesystemDirectoryCreationErrorCode : std::uint8_t
{
    forbidden,                  ///< The location policy rejects write operations.
    content_not_allowed,        ///< Directory creation received request content.
    coordination_failed,        ///< Coordinated resource ownership was not granted.
    directory_creation_failed,  ///< Confined directory creation failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a directory-creation failure.
using FilesystemDirectoryCreationErrorDetail =
    std::variant<std::monostate, FilesystemResourceAccessError, filesystem::DirectoryCreationError>;

/// @brief Preserves a stable directory-creation category and optional lower-level detail.
struct FilesystemDirectoryCreationError
{
    FilesystemDirectoryCreationErrorCode code{};     ///< Stable application failure category.
    FilesystemDirectoryCreationErrorDetail detail{}; ///< Matching lower-level error, if any.
};

/// @brief Identifies a transport-neutral filesystem-removal failure.
enum class FilesystemRemovalErrorCode : std::uint8_t
{
    forbidden,                  ///< The location policy rejects delete operations.
    content_not_allowed,        ///< Removal received request content.
    coordination_failed,        ///< Coordinated resource ownership was not granted.
    filesystem_removal_failed,  ///< Confined entry removal failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a filesystem-removal failure.
using FilesystemRemovalErrorDetail = std::variant<std::monostate, FilesystemResourceAccessError,
                                                  filesystem::FilesystemEntryRemovalError>;

/// @brief Preserves a stable removal category and optional lower-level failure detail.
struct FilesystemRemovalError
{
    FilesystemRemovalErrorCode code{};     ///< Stable application failure category.
    FilesystemRemovalErrorDetail detail{}; ///< Matching lower-level error when applicable.
};

/// @brief Retains a stable file stream and its shared resource lease.
class FilesystemFileRead final
{
  public:
    FilesystemFileRead(const FilesystemFileRead &) = delete;
    FilesystemFileRead &operator=(const FilesystemFileRead &) = delete;
    /// @brief Transfers the stable stream and shared resource claim.
    /// @param[in,out] other Read resource left without ownership.
    FilesystemFileRead(FilesystemFileRead &&other) noexcept = default;
    /// @brief Replaces this read resource through an ownership transfer.
    /// @param[in,out] other Read resource left without ownership.
    /// @return This resource after the transfer.
    FilesystemFileRead &operator=(FilesystemFileRead &&other) noexcept;
    ~FilesystemFileRead() = default;

    /// @brief Returns the file length captured from the stable native handle.
    /// @return Exact byte length retained for response framing.
    [[nodiscard]] std::uint64_t size() const noexcept;
    /// @brief Reads one bounded chunk while retaining coordinated shared ownership.
    /// @param[out] destination Caller-bounded output storage.
    /// @param[in] stop_token Current streaming-operation cancellation token.
    /// @return Produced bytes, end of input, or a stable file-read failure.
    [[nodiscard]] Result<std::size_t, filesystem::FileReadError>
    read(std::span<std::byte> destination, const std::stop_token &stop_token);

  private:
    friend class FilesystemService;

    /// @brief Combines an opened file with coordinated and request-lifetime ownership.
    /// @param[in] stream Stable root-confined native file stream.
    /// @param[in] lease Shared claim retained until streaming ends.
    /// @param[in] options Original request cancellation and deadline policy.
    FilesystemFileRead(filesystem::FileReadStream stream, FilesystemResourceLease lease,
                       FilesystemResourceAccessOptions options) noexcept;

    FilesystemResourceLease lease_;           ///< Released after the stream closes.
    filesystem::FileReadStream stream_;       ///< Closed before releasing the shared claim.
    FilesystemResourceAccessOptions options_; ///< Request lifetime policy for the stream.
};

/// @brief Represents either directory metadata or a coordinated stable file stream.
using FilesystemReadResource =
    std::variant<std::vector<filesystem::DirectoryListingEntry>, FilesystemFileRead>;

/// @brief Describes decoded upload content without depending on HTTP request types.
struct FilesystemUploadContent
{
    std::span<const std::byte> memory; ///< Complete in-memory content, when retained in memory.
    std::uint64_t size{};              ///< Exact decoded content size.
    std::shared_ptr<const storage::TemporaryFile> artifact; ///< Completed file-backed content.
    storage::TemporaryFileIoOptions options; ///< Cancellation and deadline for publication.
};

/// @brief Executes one location's filesystem use cases behind a permission boundary.
class FilesystemService final
{
  public:
    /// @brief Stores one validated root and its immutable operation permissions.
    /// @param[in] root Canonical filesystem security boundary.
    /// @param[in] permissions Operations allowed within the root.
    /// @param[in] coordinator Registry shared by filesystem locations across the process runtime.
    FilesystemService(filesystem::SharedRoot root, filesystem::LocationPermissions permissions,
                      FilesystemResourceCoordinator coordinator = {});

    /// @brief Resolves a directory listing or stable file stream after one read-policy check.
    /// @param[in] requested_path URL-encoded path relative to the service root.
    /// @param[in] options Cancellation and deadline retained by a streamed file read.
    /// @return Resolved resource or a transport-neutral application failure.
    [[nodiscard]] Result<FilesystemReadResource, FilesystemReadError>
    read(std::string_view requested_path,
         const FilesystemResourceAccessOptions &options = {}) const;

    /// @brief Materializes and atomically publishes one upload after one write-policy check.
    /// @param[in] requested_path URL-encoded destination relative to the service root.
    /// @param[in] content Complete memory-backed or file-backed decoded content.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemUploadError>
    upload(std::string_view requested_path, const FilesystemUploadContent &content) const;

    /// @brief Creates one empty directory after one write-policy check.
    /// @param[in] requested_path URL-encoded destination relative to the service root.
    /// @param[in] content_size Supplied operation content; must be zero.
    /// @param[in] options Cancellation and deadline checked before acquiring ownership.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemDirectoryCreationError>
    create_directory(std::string_view requested_path, std::uint64_t content_size,
                     const FilesystemResourceAccessOptions &options = {}) const;

    /// @brief Removes one confined entry after one delete-policy check.
    /// @param[in] requested_path URL-encoded entry relative to the service root.
    /// @param[in] content_size Supplied operation content; must be zero.
    /// @param[in] options Cancellation and deadline checked before acquiring ownership.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemRemovalError>
    remove(std::string_view requested_path, std::uint64_t content_size,
           const FilesystemResourceAccessOptions &options = {}) const;

  private:
    filesystem::SharedRoot root_;                 ///< Canonical operation boundary.
    filesystem::LocationPermissions permissions_; ///< Immutable use-case permissions.
    FilesystemResourceCoordinator coordinator_;   ///< Shared in-process resource ownership.
};

} // namespace sparenode::application
