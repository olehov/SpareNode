#include "sparenode/filesystem/upload_finalizer.hpp"

#include <chrono>
#include <new>
#include <system_error>
#include <utility>

#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

namespace sparenode::filesystem
{
namespace
{

/// @brief Prevents any publication attempt after cancellation or expiry.
[[nodiscard]] std::optional<UploadFinalizationError>
interruption(const TemporaryFileIoOptions &options) noexcept
{
    if (options.stop_token.stop_requested())
    {
        return UploadFinalizationError{UploadFinalizationErrorCode::cancelled};
    }
    if (options.deadline && std::chrono::steady_clock::now() >= options.deadline.value())
    {
        return UploadFinalizationError{UploadFinalizationErrorCode::deadline_exceeded};
    }
    return std::nullopt;
}

} // namespace

/// @brief Validates the destination and holds its confined parent during publication.
Result<std::filesystem::path, UploadFinalizationError>
finalize_upload(const configuration::SharedRoot &shared_root, const std::string_view requested_path,
                const TemporaryFile &source, const TemporaryFileIoOptions &options)
{
    if (!source.completed() || source.path().empty())
    {
        return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::invalid_source});
    }
    if (const auto stopped = interruption(options); stopped)
    {
        return unexpected(stopped.value());
    }
    try
    {
        auto destination = SafePath::resolve(shared_root, requested_path);
        if (!destination)
        {
            return unexpected(UploadFinalizationError{
                UploadFinalizationErrorCode::invalid_destination, destination.error()});
        }
        const auto &path = destination->path();
        const auto native_name = path.filename();
        if (path == shared_root.path() || native_name.empty() || native_name == "." ||
            native_name == "..")
        {
            return unexpected(
                UploadFinalizationError{UploadFinalizationErrorCode::invalid_destination});
        }
        auto parent = detail::open_confined_directory(shared_root.path(), path.parent_path());
        if (!parent)
        {
            return unexpected(UploadFinalizationError{
                UploadFinalizationErrorCode::parent_unavailable, {}, parent.error()});
        }
        if (auto published =
                detail::publish_confined_upload(parent.value(), source, native_name, options);
            !published)
        {
            return unexpected(published.error());
        }
        return path;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::resource_failure});
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        return unexpected(
            UploadFinalizationError{UploadFinalizationErrorCode::publish_failed, {}, error.code()});
    }
    catch (const std::system_error &error)
    {
        return unexpected(
            UploadFinalizationError{UploadFinalizationErrorCode::publish_failed, {}, error.code()});
    }
}

/// @brief Maps one finalization category to its stable English diagnostic.
const char *to_string(const UploadFinalizationErrorCode code) noexcept
{
    switch (code)
    {
    case UploadFinalizationErrorCode::invalid_source:
        return "upload source is not a completed temporary file";
    case UploadFinalizationErrorCode::invalid_destination:
        return "upload destination is invalid";
    case UploadFinalizationErrorCode::parent_unavailable:
        return "upload destination parent is unavailable or unconfined";
    case UploadFinalizationErrorCode::destination_exists:
        return "upload destination already exists";
    case UploadFinalizationErrorCode::source_read_failed:
        return "completed upload source could not be read";
    case UploadFinalizationErrorCode::staging_failed:
        return "private upload staging file could not be created";
    case UploadFinalizationErrorCode::staging_write_failed:
        return "private upload staging write failed";
    case UploadFinalizationErrorCode::staging_flush_failed:
        return "private upload staging flush failed";
    case UploadFinalizationErrorCode::publish_failed:
        return "upload could not be published atomically";
    case UploadFinalizationErrorCode::cancelled:
        return "upload finalization was cancelled";
    case UploadFinalizationErrorCode::deadline_exceeded:
        return "upload finalization deadline expired";
    case UploadFinalizationErrorCode::resource_failure:
        return "upload finalization exhausted resources";
    }
    return "unknown upload finalization error";
}

} // namespace sparenode::filesystem
