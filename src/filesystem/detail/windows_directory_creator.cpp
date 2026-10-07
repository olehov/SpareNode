#include "directory_creator.hpp"

#ifdef _WIN32

#include <limits>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include "windows_handle.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Converts one NT failure into the portable creation contract.
[[nodiscard]] DirectoryCreationError failure(const NTSTATUS status) noexcept
{
    const auto native_error = RtlNtStatusToDosError(status);
    auto code = DirectoryCreationErrorCode::creation_failed;
    if (native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS)
    {
        code = DirectoryCreationErrorCode::destination_exists;
    }
    else if (native_error == ERROR_PATH_NOT_FOUND || native_error == ERROR_DIRECTORY)
    {
        code = DirectoryCreationErrorCode::parent_unavailable;
    }
    else if (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_PRIVILEGE_NOT_HELD ||
             native_error == ERROR_WRITE_PROTECT)
    {
        code = DirectoryCreationErrorCode::permission_denied;
    }
    return {code, {}, {static_cast<int>(native_error), std::system_category()}};
}

} // namespace

/// @brief Creates one child directory through a stable Windows parent handle.
Result<void, DirectoryCreationError>
create_confined_directory(const ConfinedDirectory &parent, const std::filesystem::path &native_name)
{
    const auto &name = native_name.native();
    constexpr auto maximum_name_bytes =
        static_cast<std::size_t>(std::numeric_limits<USHORT>::max());
    const auto name_bytes = name.size() * sizeof(wchar_t);
    if (name.empty() || native_name != native_name.filename() || name_bytes > maximum_name_bytes)
    {
        return unexpected(DirectoryCreationError{DirectoryCreationErrorCode::invalid_destination});
    }

    UNICODE_STRING object_name{};
    object_name.Length = static_cast<USHORT>(name_bytes);
    object_name.MaximumLength = object_name.Length;
    object_name.Buffer = const_cast<PWSTR>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE,
                               parent.native_handle(), nullptr);
    IO_STATUS_BLOCK status_block{};
    HANDLE directory = INVALID_HANDLE_VALUE;
    const auto status = NtCreateFile(
        &directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes,
        &status_block, nullptr, FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
        FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr,
        0);
    if (status < 0)
    {
        return unexpected(failure(status));
    }
    const UniqueWindowsHandle created(directory);
    return {};
}

} // namespace sparenode::filesystem::detail

#endif
