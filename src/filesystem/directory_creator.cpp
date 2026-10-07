#include "sparenode/filesystem/directory_creator.hpp"

#include <new>
#include <system_error>
#include <utility>

#include "detail/directory_creator.hpp"
#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

namespace sparenode::filesystem
{

/// @brief Validates a new directory path and creates it relative to a confined parent.
Result<std::filesystem::path, DirectoryCreationError>
create_directory(const configuration::SharedRoot &shared_root,
                 const std::string_view requested_path)
{
    try
    {
        auto destination = SafePath::resolve(shared_root, requested_path);
        if (!destination)
        {
            return unexpected(DirectoryCreationError{
                DirectoryCreationErrorCode::invalid_destination, destination.error()});
        }
        const auto &path = destination->path();
        const auto native_name = path.filename();
        if (path == shared_root.path() || native_name.empty() || native_name == "." ||
            native_name == "..")
        {
            return unexpected(
                DirectoryCreationError{DirectoryCreationErrorCode::invalid_destination});
        }

        auto parent = detail::open_confined_directory(shared_root.path(), path.parent_path());
        if (!parent)
        {
            return unexpected(DirectoryCreationError{
                DirectoryCreationErrorCode::parent_unavailable, {}, parent.error()});
        }
        if (auto created = detail::create_confined_directory(parent.value(), native_name); !created)
        {
            return unexpected(created.error());
        }
        return path;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(DirectoryCreationError{DirectoryCreationErrorCode::resource_failure});
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        return unexpected(
            DirectoryCreationError{DirectoryCreationErrorCode::creation_failed, {}, error.code()});
    }
    catch (const std::system_error &error)
    {
        return unexpected(
            DirectoryCreationError{DirectoryCreationErrorCode::creation_failed, {}, error.code()});
    }
}

/// @brief Maps one creation category to stable English diagnostic text.
const char *to_string(const DirectoryCreationErrorCode code) noexcept
{
    switch (code)
    {
    case DirectoryCreationErrorCode::invalid_destination:
        return "directory destination is invalid";
    case DirectoryCreationErrorCode::parent_unavailable:
        return "directory destination parent is unavailable or unconfined";
    case DirectoryCreationErrorCode::destination_exists:
        return "directory destination already exists";
    case DirectoryCreationErrorCode::permission_denied:
        return "directory creation permission was denied";
    case DirectoryCreationErrorCode::creation_failed:
        return "directory could not be created";
    case DirectoryCreationErrorCode::resource_failure:
        return "directory creation exhausted resources";
    }
    return "unknown directory creation error";
}

} // namespace sparenode::filesystem
