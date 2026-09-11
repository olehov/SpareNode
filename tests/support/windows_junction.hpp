#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace sparenode::test
{
namespace detail
{

inline constexpr std::size_t reparse_data_header_size = 8;

/// @brief Storage layout accepted by FSCTL_SET_REPARSE_POINT for a mount point.
struct JunctionReparseData
{
    DWORD tag{};
    WORD data_length{};
    WORD reserved{};
    WORD substitute_offset{};
    WORD substitute_length{};
    WORD print_offset{};
    WORD print_length{};
    wchar_t path_buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t)]{};
};

/// @brief Minimal GUID-form buffer used to create a test-only third-party tag.
struct UnsupportedReparseData
{
    DWORD tag{};
    WORD data_length{};
    WORD reserved{};
    GUID guid{};
};

} // namespace detail

/// @brief Converts an absolute DOS/UNC path to exact extended-length Win32 syntax.
/// @param[in] path Absolute Windows path whose spelling must be preserved.
/// @return Equivalent `\\?\` path that disables legacy name normalization.
[[nodiscard]] inline std::filesystem::path extended_windows_path(const std::filesystem::path &path)
{
    constexpr std::wstring_view extended_prefix = LR"(\\?\)";
    constexpr std::wstring_view unc_prefix = LR"(\\)";
    const auto &native = path.native();
    if (native.starts_with(extended_prefix))
    {
        return path;
    }
    if (native.starts_with(unc_prefix))
    {
        return std::filesystem::path(std::wstring(LR"(\\?\UNC\)") + native.substr(2));
    }
    return std::filesystem::path(std::wstring(extended_prefix) + native);
}

/// @brief Creates an NTFS directory junction without requiring a shell process.
/// @param[in] target Existing absolute directory targeted by the junction.
/// @param[in] junction New directory path that becomes the junction.
/// @return Empty error code on success or the corresponding Win32 failure.
[[nodiscard]] inline std::error_code
create_directory_junction(const std::filesystem::path &target,
                          const std::filesystem::path &junction)
{
    std::error_code error;
    if (!std::filesystem::create_directory(junction, error))
    {
        return error ? error : std::make_error_code(std::errc::file_exists);
    }

    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const auto handle =
        CreateFileW(junction.c_str(), GENERIC_WRITE, sharing, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const auto result =
            std::error_code(static_cast<int>(GetLastError()), std::system_category());
        std::filesystem::remove(junction, error);
        return result;
    }

    detail::JunctionReparseData data{};
    const auto print_name = target.native();
    const auto substitute_name = std::wstring(LR"(\??\)") + print_name;
    const auto substitute_bytes = substitute_name.size() * sizeof(wchar_t);
    const auto print_bytes = print_name.size() * sizeof(wchar_t);
    const auto print_offset = substitute_bytes + sizeof(wchar_t);
    const auto total_size = offsetof(detail::JunctionReparseData, path_buffer) + print_offset +
                            print_bytes + sizeof(wchar_t);
    if (total_size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE ||
        total_size - detail::reparse_data_header_size > std::numeric_limits<WORD>::max())
    {
        static_cast<void>(CloseHandle(handle));
        std::filesystem::remove(junction, error);
        return std::make_error_code(std::errc::filename_too_long);
    }

    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.data_length = static_cast<WORD>(total_size - detail::reparse_data_header_size);
    data.substitute_length = static_cast<WORD>(substitute_bytes);
    data.print_offset = static_cast<WORD>(print_offset);
    data.print_length = static_cast<WORD>(print_bytes);
    std::memcpy(data.path_buffer, substitute_name.data(), substitute_bytes);
    std::memcpy(reinterpret_cast<std::byte *>(data.path_buffer) + print_offset, print_name.data(),
                print_bytes);

    DWORD returned_bytes = 0;
    const auto created =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, static_cast<DWORD>(total_size),
                        nullptr, 0, &returned_bytes, nullptr);
    const auto native_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    static_cast<void>(CloseHandle(handle));
    if (native_error != ERROR_SUCCESS)
    {
        std::filesystem::remove(junction, error);
    }
    return std::error_code(static_cast<int>(native_error), std::system_category());
}

/// @brief Creates a test reparse point with an unsupported third-party tag.
/// @param[in] path New empty directory that receives the test tag.
/// @return Empty error code on success or the corresponding Win32 failure.
[[nodiscard]] inline std::error_code
create_unsupported_directory_reparse_point(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::create_directory(path, error))
    {
        return error ? error : std::make_error_code(std::errc::file_exists);
    }
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const auto handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, sharing, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const auto result =
            std::error_code(static_cast<int>(GetLastError()), std::system_category());
        std::filesystem::remove(path, error);
        return result;
    }

    detail::UnsupportedReparseData data{};
    data.tag = 0x42; // Valid non-Microsoft tag reserved exclusively for this isolated test.
    data.guid = {0x8e7059d3, 0x70fe, 0x4ae1, {0xa6, 0x5a, 0xed, 0x97, 0x8f, 0x5a, 0x31, 0x48}};
    DWORD returned_bytes = 0;
    const auto created =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, static_cast<DWORD>(sizeof(data)),
                        nullptr, 0, &returned_bytes, nullptr);
    const auto native_error = created != FALSE ? ERROR_SUCCESS : GetLastError();
    static_cast<void>(CloseHandle(handle));
    if (native_error != ERROR_SUCCESS)
    {
        std::filesystem::remove(path, error);
    }
    return std::error_code(static_cast<int>(native_error), std::system_category());
}

} // namespace sparenode::test

#endif
