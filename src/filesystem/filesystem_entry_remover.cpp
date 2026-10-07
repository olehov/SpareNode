#include "sparenode/filesystem/filesystem_entry_remover.hpp"

#include <filesystem>
#include <new>
#include <string>
#include <system_error>

#include "sparenode/filesystem/detail/directory_entry_reader.hpp"
#include "sparenode/filesystem/detail/path_request_decoder.hpp"

namespace sparenode::filesystem
{
namespace
{

/// @brief Reconstructs the validated lexical path so the final link itself can be deleted.
[[nodiscard]] std::filesystem::path
lexical_destination(const configuration::SharedRoot &shared_root,
                    const std::string_view requested_path)
{
    const auto decoded = detail::decode_path_request(requested_path);
    if (!decoded)
    {
        return {};
    }
    const std::u8string utf8_path(decoded->begin(), decoded->end());
    const auto relative_path = std::filesystem::path(utf8_path).lexically_normal();
    return relative_path.empty() || relative_path == "."
               ? shared_root.path()
               : (shared_root.path() / relative_path).lexically_normal();
}

} // namespace

/// @brief Validates and removes one entry relative to its confined parent.
Result<void, FilesystemEntryRemovalError>
remove_filesystem_entry(const configuration::SharedRoot &shared_root,
                        const std::string_view requested_path)
{
    try
    {
        const auto destination = SafePath::resolve(shared_root, requested_path);
        if (!destination)
        {
            return unexpected(FilesystemEntryRemovalError{
                FilesystemEntryRemovalErrorCode::invalid_destination, destination.error()});
        }

        const auto path = lexical_destination(shared_root, requested_path);
        const auto native_name = path.filename();
        if (path.empty() || path == shared_root.path() || native_name.empty() ||
            native_name == "." || native_name == "..")
        {
            return unexpected(
                FilesystemEntryRemovalError{FilesystemEntryRemovalErrorCode::invalid_destination});
        }

        auto parent =
            detail::open_confined_mutation_directory(shared_root.path(), path.parent_path());
        if (!parent)
        {
            return unexpected(FilesystemEntryRemovalError{
                FilesystemEntryRemovalErrorCode::parent_unavailable, {}, parent.error()});
        }
        return detail::remove_confined_entry(parent.value(), native_name);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            FilesystemEntryRemovalError{FilesystemEntryRemovalErrorCode::resource_failure});
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        return unexpected(FilesystemEntryRemovalError{
            FilesystemEntryRemovalErrorCode::removal_failed, {}, error.code()});
    }
    catch (const std::system_error &error)
    {
        return unexpected(FilesystemEntryRemovalError{
            FilesystemEntryRemovalErrorCode::removal_failed, {}, error.code()});
    }
}

const char *to_string(const FilesystemEntryRemovalErrorCode code) noexcept
{
    switch (code)
    {
    case FilesystemEntryRemovalErrorCode::invalid_destination:
        return "filesystem deletion destination is invalid";
    case FilesystemEntryRemovalErrorCode::parent_unavailable:
        return "filesystem deletion parent is unavailable or unconfined";
    case FilesystemEntryRemovalErrorCode::not_found:
        return "filesystem deletion destination does not exist";
    case FilesystemEntryRemovalErrorCode::directory_not_empty:
        return "recursive directory deletion is unsupported";
    case FilesystemEntryRemovalErrorCode::permission_denied:
        return "filesystem deletion permission was denied";
    case FilesystemEntryRemovalErrorCode::removal_failed:
        return "filesystem entry could not be deleted";
    case FilesystemEntryRemovalErrorCode::resource_failure:
        return "filesystem deletion exhausted resources";
    }
    return "unknown filesystem deletion error";
}

} // namespace sparenode::filesystem
