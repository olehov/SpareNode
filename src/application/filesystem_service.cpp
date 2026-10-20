#include "sparenode/application/filesystem_service.hpp"

#include <new>
#include <optional>
#include <utility>

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

} // namespace

FilesystemService::FilesystemService(filesystem::SharedRoot root,
                                     const filesystem::LocationPermissions permissions) noexcept
    : root_(std::move(root)), permissions_(permissions)
{
}

Result<FilesystemReadResource, FilesystemReadError>
FilesystemService::read(const std::string_view requested_path) const
{
    if (!permissions_.allows_read())
    {
        return unexpected(service_error<FilesystemReadError>(FilesystemReadErrorCode::forbidden));
    }
    try
    {
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
        return FilesystemReadResource(std::in_place_type<filesystem::FileReadStream>,
                                      std::move(file).value());
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
                                    const std::uint64_t content_size) const
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
FilesystemService::remove(const std::string_view requested_path,
                          const std::uint64_t content_size) const
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
