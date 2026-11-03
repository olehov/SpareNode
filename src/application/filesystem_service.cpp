#include "sparenode/application/filesystem_service.hpp"

#include <new>
#include <optional>
#include <utility>

#include "sparenode/foundation/concurrency/interruption.hpp"

namespace sparenode::application
{
namespace
{

/// @brief Creates a typed application failure without lower-level detail.
template <typename Error, typename ErrorCode>
[[nodiscard]] Error service_error(const ErrorCode code) noexcept
{
    return {code, {}};
}

/// @brief Creates a typed application failure retaining its lower-level cause.
template <typename Error, typename ErrorCode, typename Detail>
[[nodiscard]] Error service_error(const ErrorCode code, Detail detail)
{
    return {code, std::move(detail)};
}

/// @brief Preserves the established read error contract for an invalid coordinated path.
[[nodiscard]] FilesystemReadError read_coordination_error(FilesystemResourceAccessError error)
{
    if (error.code == FilesystemResourceAccessErrorCode::invalid_path && error.path_error)
    {
        return service_error<FilesystemReadError>(
            FilesystemReadErrorCode::directory_listing_failed,
            filesystem::DirectoryListingError{
                filesystem::DirectoryListingErrorCode::invalid_path, {}, error.path_error->code});
    }
    return service_error<FilesystemReadError>(FilesystemReadErrorCode::coordination_failed,
                                              std::move(error));
}

/// @brief Preserves the established upload error contract for an invalid coordinated path.
[[nodiscard]] FilesystemUploadError upload_coordination_error(FilesystemResourceAccessError error)
{
    if (error.code == FilesystemResourceAccessErrorCode::invalid_path && error.path_error)
    {
        return service_error<FilesystemUploadError>(
            FilesystemUploadErrorCode::upload_finalization_failed,
            filesystem::UploadFinalizationError{
                filesystem::UploadFinalizationErrorCode::invalid_destination,
                std::move(error.path_error),
                {}});
    }
    return service_error<FilesystemUploadError>(FilesystemUploadErrorCode::coordination_failed,
                                                std::move(error));
}

/// @brief Preserves the established creation error contract for an invalid coordinated path.
[[nodiscard]] FilesystemDirectoryCreationError
creation_coordination_error(FilesystemResourceAccessError error)
{
    if (error.code == FilesystemResourceAccessErrorCode::invalid_path && error.path_error)
    {
        return service_error<FilesystemDirectoryCreationError>(
            FilesystemDirectoryCreationErrorCode::directory_creation_failed,
            filesystem::DirectoryCreationError{
                filesystem::DirectoryCreationErrorCode::invalid_destination,
                std::move(error.path_error),
                {}});
    }
    return service_error<FilesystemDirectoryCreationError>(
        FilesystemDirectoryCreationErrorCode::coordination_failed, std::move(error));
}

/// @brief Preserves the established removal error contract for an invalid coordinated path.
[[nodiscard]] FilesystemRemovalError removal_coordination_error(FilesystemResourceAccessError error)
{
    if (error.code == FilesystemResourceAccessErrorCode::invalid_path && error.path_error)
    {
        return service_error<FilesystemRemovalError>(
            FilesystemRemovalErrorCode::filesystem_removal_failed,
            filesystem::FilesystemEntryRemovalError{
                filesystem::FilesystemEntryRemovalErrorCode::invalid_destination,
                std::move(error.path_error),
                {}});
    }
    return service_error<FilesystemRemovalError>(FilesystemRemovalErrorCode::coordination_failed,
                                                 std::move(error));
}

} // namespace

FilesystemFileRead::FilesystemFileRead(filesystem::FileReadStream stream,
                                       FilesystemResourceLease lease,
                                       FilesystemResourceAccessOptions options) noexcept
    : lease_(std::move(lease)), stream_(std::move(stream)), options_(std::move(options))
{
}

FilesystemFileRead &FilesystemFileRead::operator=(FilesystemFileRead &&other) noexcept
{
    if (this != &other)
    {
        // Close the old descriptor while its claim is still held, then transfer the new claim.
        stream_ = std::move(other.stream_);
        lease_ = std::move(other.lease_);
        options_ = std::move(other.options_);
    }
    return *this;
}

std::uint64_t FilesystemFileRead::size() const noexcept
{
    return stream_.size();
}

Result<std::size_t, filesystem::FileReadError>
FilesystemFileRead::read(const std::span<std::byte> destination,
                         const FilesystemResourceAccessOptions &options)
{
    if (concurrency::interruption_status(options.stop_token, options.deadline) !=
            concurrency::InterruptionStatus::none ||
        concurrency::interruption_status(options_.stop_token, options_.deadline) !=
            concurrency::InterruptionStatus::none)
    {
        return unexpected(
            filesystem::FileReadError{filesystem::FileReadErrorCode::cancelled, {}, {}});
    }
    return stream_.read(destination, options.stop_token);
}

FilesystemService::FilesystemService(filesystem::SharedRoot root,
                                     const filesystem::LocationPermissions permissions,
                                     FilesystemResourceCoordinator coordinator)
    : root_(std::move(root)), permissions_(permissions), coordinator_(std::move(coordinator))
{
}

Result<FilesystemReadResource, FilesystemReadError>
FilesystemService::read(const std::string_view requested_path,
                        const FilesystemResourceAccessOptions &options) const
{
    if (!permissions_.allows_read())
    {
        return unexpected(service_error<FilesystemReadError>(FilesystemReadErrorCode::forbidden));
    }
    try
    {
        auto lease = coordinator_.acquire(root_, requested_path,
                                          FilesystemResourceAccessMode::shared_read, options);
        if (!lease)
        {
            return unexpected(read_coordination_error(std::move(lease).error()));
        }
        auto listing = filesystem::list_directory(root_, requested_path);
        if (listing)
        {
            return FilesystemReadResource(
                std::in_place_type<std::vector<filesystem::DirectoryListingEntry>>,
                std::move(listing).value());
        }
        if (listing.error().code != filesystem::DirectoryListingErrorCode::not_directory)
        {
            return unexpected(service_error<FilesystemReadError>(
                FilesystemReadErrorCode::directory_listing_failed, listing.error()));
        }

        auto file = filesystem::FileReadStream::open(root_, requested_path);
        if (!file)
        {
            return unexpected(service_error<FilesystemReadError>(
                FilesystemReadErrorCode::file_read_failed, file.error()));
        }
        FilesystemFileRead coordinated_file(std::move(file).value(), std::move(lease).value(),
                                            options);
        return FilesystemReadResource(std::in_place_type<FilesystemFileRead>,
                                      std::move(coordinated_file));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(service_error<FilesystemReadError>(
            FilesystemReadErrorCode::resource_allocation_failed));
    }
}

Result<void, FilesystemUploadError>
FilesystemService::upload(const std::string_view requested_path,
                          const FilesystemUploadContent &content) const
{
    if (!permissions_.allows_write())
    {
        return unexpected(
            service_error<FilesystemUploadError>(FilesystemUploadErrorCode::forbidden));
    }
    try
    {
        const FilesystemResourceAccessOptions access_options{content.options.stop_token,
                                                             content.options.deadline};
        auto lease =
            coordinator_.acquire(root_, requested_path,
                                 FilesystemResourceAccessMode::exclusive_mutation, access_options);
        if (!lease)
        {
            return unexpected(upload_coordination_error(std::move(lease).error()));
        }
        std::optional<storage::TemporaryFile> materialized_artifact;
        const storage::TemporaryFile *source = content.artifact.get();
        if (source == nullptr)
        {
            if (content.size != content.memory.size())
            {
                return unexpected(service_error<FilesystemUploadError>(
                    FilesystemUploadErrorCode::invalid_upload));
            }
            auto created = storage::TemporaryFile::create();
            if (!created)
            {
                return unexpected(service_error<FilesystemUploadError>(
                    FilesystemUploadErrorCode::temporary_file_failed, created.error()));
            }
            materialized_artifact.emplace(std::move(created).value());
            if (auto written = materialized_artifact->write(content.memory, content.options);
                !written)
            {
                return unexpected(service_error<FilesystemUploadError>(
                    FilesystemUploadErrorCode::temporary_file_failed, written.error()));
            }
            if (auto completed = materialized_artifact->complete(content.options); !completed)
            {
                return unexpected(service_error<FilesystemUploadError>(
                    FilesystemUploadErrorCode::temporary_file_failed, completed.error()));
            }
            source = &materialized_artifact.value();
        }

        auto published =
            filesystem::finalize_upload(root_, requested_path, *source, content.options);
        if (!published)
        {
            return unexpected(service_error<FilesystemUploadError>(
                FilesystemUploadErrorCode::upload_finalization_failed, published.error()));
        }
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(service_error<FilesystemUploadError>(
            FilesystemUploadErrorCode::resource_allocation_failed));
    }
}

Result<void, FilesystemDirectoryCreationError>
FilesystemService::create_directory(const std::string_view requested_path,
                                    const std::uint64_t content_size,
                                    const FilesystemResourceAccessOptions &options) const
{
    if (!permissions_.allows_write())
    {
        return unexpected(service_error<FilesystemDirectoryCreationError>(
            FilesystemDirectoryCreationErrorCode::forbidden));
    }
    if (content_size != 0)
    {
        return unexpected(service_error<FilesystemDirectoryCreationError>(
            FilesystemDirectoryCreationErrorCode::content_not_allowed));
    }
    try
    {
        auto lease = coordinator_.acquire(
            root_, requested_path, FilesystemResourceAccessMode::exclusive_mutation, options);
        if (!lease)
        {
            return unexpected(creation_coordination_error(std::move(lease).error()));
        }
        auto created = filesystem::create_directory(root_, requested_path);
        if (!created)
        {
            return unexpected(service_error<FilesystemDirectoryCreationError>(
                FilesystemDirectoryCreationErrorCode::directory_creation_failed, created.error()));
        }
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(service_error<FilesystemDirectoryCreationError>(
            FilesystemDirectoryCreationErrorCode::resource_allocation_failed));
    }
}

Result<void, FilesystemRemovalError>
FilesystemService::remove(const std::string_view requested_path, const std::uint64_t content_size,
                          const FilesystemResourceAccessOptions &options) const
{
    if (!permissions_.allows_delete())
    {
        return unexpected(
            service_error<FilesystemRemovalError>(FilesystemRemovalErrorCode::forbidden));
    }
    if (content_size != 0)
    {
        return unexpected(
            service_error<FilesystemRemovalError>(FilesystemRemovalErrorCode::content_not_allowed));
    }
    try
    {
        auto lease = coordinator_.acquire(
            root_, requested_path, FilesystemResourceAccessMode::exclusive_mutation, options);
        if (!lease)
        {
            return unexpected(removal_coordination_error(std::move(lease).error()));
        }
        auto removed = filesystem::remove_filesystem_entry(root_, requested_path);
        if (!removed)
        {
            return unexpected(service_error<FilesystemRemovalError>(
                FilesystemRemovalErrorCode::filesystem_removal_failed, removed.error()));
        }
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(service_error<FilesystemRemovalError>(
            FilesystemRemovalErrorCode::resource_allocation_failed));
    }
}

} // namespace sparenode::application
