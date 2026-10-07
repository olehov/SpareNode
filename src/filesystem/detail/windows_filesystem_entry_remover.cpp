#include "filesystem_entry_remover.hpp"

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

/// @brief Converts one Win32 failure into the portable deletion contract.
[[nodiscard]] FilesystemEntryRemovalError
failure_from_windows_error(const DWORD native_error) noexcept
{
    auto code = FilesystemEntryRemovalErrorCode::removal_failed;
    if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND ||
        native_error == ERROR_DIRECTORY)
    {
        code = FilesystemEntryRemovalErrorCode::not_found;
    }
    else if (native_error == ERROR_DIR_NOT_EMPTY || native_error == ERROR_BUSY)
    {
        code = FilesystemEntryRemovalErrorCode::directory_not_empty;
    }
    else if (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_SHARING_VIOLATION ||
             native_error == ERROR_WRITE_PROTECT)
    {
        code = FilesystemEntryRemovalErrorCode::permission_denied;
    }
    return {code, {}, {static_cast<int>(native_error), std::system_category()}};
}

/// @brief Converts one NT failure through its corresponding Win32 error.
[[nodiscard]] FilesystemEntryRemovalError failure(const NTSTATUS status) noexcept
{
    return failure_from_windows_error(RtlNtStatusToDosError(status));
}

} // namespace

Result<void, FilesystemEntryRemovalError>
remove_confined_entry(const ConfinedDirectory &parent, const std::filesystem::path &native_name)
{
    const auto &name = native_name.native();
    constexpr auto maximum_name_bytes =
        static_cast<std::size_t>(std::numeric_limits<USHORT>::max());
    const auto name_bytes = name.size() * sizeof(wchar_t);
    if (name.empty() || native_name != native_name.filename() || name_bytes > maximum_name_bytes)
    {
        return unexpected(
            FilesystemEntryRemovalError{FilesystemEntryRemovalErrorCode::invalid_destination});
    }

    UNICODE_STRING object_name{};
    object_name.Length = static_cast<USHORT>(name_bytes);
    object_name.MaximumLength = object_name.Length;
    object_name.Buffer = const_cast<PWSTR>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE,
                               parent.native_handle(), nullptr);
    IO_STATUS_BLOCK status_block{};
    HANDLE entry = INVALID_HANDLE_VALUE;
    const auto open_status =
        NtCreateFile(&entry, DELETE | SYNCHRONIZE, &attributes, &status_block, nullptr, 0,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                     FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (open_status < 0)
    {
        return unexpected(failure(open_status));
    }
    const UniqueWindowsHandle stable_entry(entry);
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(stable_entry.get(), FileDispositionInfo, &disposition,
                                   sizeof(disposition)) == FALSE)
    {
        return unexpected(failure_from_windows_error(GetLastError()));
    }
    return {};
}

} // namespace sparenode::filesystem::detail

#endif
