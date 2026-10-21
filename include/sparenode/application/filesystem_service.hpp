#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

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
    directory_listing_failed,   ///< Directory listing failed below the application boundary.
    file_read_failed,           ///< File opening failed below the application boundary.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a filesystem read failure.
using FilesystemReadErrorDetail =
    std::variant<std::monostate, filesystem::DirectoryListingError, filesystem::FileReadError>;

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
    temporary_file_failed,      ///< Upload materialization failed in temporary storage.
    upload_finalization_failed, ///< Atomic upload publication failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with an upload failure.
using FilesystemUploadErrorDetail =
    std::variant<std::monostate, storage::TemporaryFileError, filesystem::UploadFinalizationError>;

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
    directory_creation_failed,  ///< Confined directory creation failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a directory-creation failure.
using FilesystemDirectoryCreationErrorDetail =
    std::variant<std::monostate, filesystem::DirectoryCreationError>;

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
    filesystem_removal_failed,  ///< Confined entry removal failed.
    resource_allocation_failed, ///< Application-service memory allocation failed.
};

/// @brief Holds the lower-level detail associated with a filesystem-removal failure.
using FilesystemRemovalErrorDetail =
    std::variant<std::monostate, filesystem::FilesystemEntryRemovalError>;

/// @brief Preserves a stable removal category and optional lower-level failure detail.
struct FilesystemRemovalError
{
    FilesystemRemovalErrorCode code{};     ///< Stable application failure category.
    FilesystemRemovalErrorDetail detail{}; ///< Matching lower-level error when applicable.
};

/// @brief Represents either directory metadata or an opened stable file stream.
using FilesystemReadResource =
    std::variant<std::vector<filesystem::DirectoryListingEntry>, filesystem::FileReadStream>;

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
    FilesystemService(filesystem::SharedRoot root,
                      filesystem::LocationPermissions permissions) noexcept;

    /// @brief Resolves a directory listing or stable file stream after one read-policy check.
    /// @param[in] requested_path URL-encoded path relative to the service root.
    /// @return Resolved resource or a transport-neutral application failure.
    [[nodiscard]] Result<FilesystemReadResource, FilesystemReadError>
    read(std::string_view requested_path) const;

    /// @brief Materializes and atomically publishes one upload after one write-policy check.
    /// @param[in] requested_path URL-encoded destination relative to the service root.
    /// @param[in] content Complete memory-backed or file-backed decoded content.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemUploadError>
    upload(std::string_view requested_path, const FilesystemUploadContent &content) const;

    /// @brief Creates one empty directory after one write-policy check.
    /// @param[in] requested_path URL-encoded destination relative to the service root.
    /// @param[in] content_size Supplied operation content; must be zero.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemDirectoryCreationError>
    create_directory(std::string_view requested_path, std::uint64_t content_size) const;

    /// @brief Removes one confined entry after one delete-policy check.
    /// @param[in] requested_path URL-encoded entry relative to the service root.
    /// @param[in] content_size Supplied operation content; must be zero.
    /// @return Success or a transport-neutral application failure.
    [[nodiscard]] Result<void, FilesystemRemovalError> remove(std::string_view requested_path,
                                                              std::uint64_t content_size) const;

  private:
    filesystem::SharedRoot root_;                 ///< Canonical operation boundary.
    filesystem::LocationPermissions permissions_; ///< Immutable use-case permissions.
};

} // namespace sparenode::application
