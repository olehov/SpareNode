#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Closes an owned Win32 filesystem handle.
struct HandleCloser
{
    /// @brief Releases a valid handle returned by CreateFileW.
    void operator()(void *handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            static_cast<void>(CloseHandle(handle));
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

/// @brief Captures the current Win32 error as a system-category error code.
[[nodiscard]] std::error_code last_system_error() noexcept
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

/// @brief Opens a filesystem object with optional final reparse traversal suppression.
[[nodiscard]] Result<UniqueHandle, std::error_code> open_path(const std::filesystem::path &path,
                                                              const bool open_reparse_point)
{
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    auto flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (open_reparse_point)
    {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return unexpected(last_system_error());
    }
    return UniqueHandle(handle);
}

/// @brief Opens one basename relative to a retained directory handle.
[[nodiscard]] Result<UniqueHandle, std::error_code>
open_relative(const HANDLE directory, const std::filesystem::path &native_name,
              const bool open_reparse_point)
{
    const auto &name = native_name.native();
    constexpr auto maximum_name_bytes =
        static_cast<std::size_t>(std::numeric_limits<USHORT>::max());
    const auto name_bytes = name.size() * sizeof(wchar_t);
    if (name.empty() || name_bytes > maximum_name_bytes)
    {
        return unexpected(std::make_error_code(std::errc::filename_too_long));
    }

    UNICODE_STRING object_name{};
    object_name.Length = static_cast<USHORT>(name_bytes);
    object_name.MaximumLength = object_name.Length;
    object_name.Buffer = const_cast<PWSTR>(name.data());

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE, directory, nullptr);
    IO_STATUS_BLOCK status_block{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    ULONG options = FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT;
    if (open_reparse_point)
    {
        options |= FILE_OPEN_REPARSE_POINT;
    }
    const auto status =
        NtOpenFile(&handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &status_block,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, options);
    if (status < 0)
    {
        return unexpected(std::error_code(static_cast<int>(RtlNtStatusToDosError(status)),
                                          std::system_category()));
    }
    return UniqueHandle(handle);
}

/// @brief Reads the normalized final path owned by a stable Win32 handle.
[[nodiscard]] Result<std::filesystem::path, std::error_code> query_final_path(const HANDLE handle)
{
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    constexpr std::size_t initial_capacity = 512;
    std::vector<wchar_t> buffer(initial_capacity);
    auto length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length == 0)
    {
        return unexpected(last_system_error());
    }
    if (length >= buffer.size())
    {
        buffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           flags);
        if (length == 0 || length >= buffer.size())
        {
            return unexpected(last_system_error());
        }
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).lexically_normal();
}

/// @brief Compares Windows path components with the filesystem's case-insensitive rules.
[[nodiscard]] bool components_equal(const std::filesystem::path &left,
                                    const std::filesystem::path &right) noexcept
{
    const auto &left_text = left.native();
    const auto &right_text = right.native();
    return CompareStringOrdinal(left_text.c_str(), static_cast<int>(left_text.size()),
                                right_text.c_str(), static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}

/// @brief Checks component-wise containment for normalized Windows paths.
[[nodiscard]] bool is_within_root(const std::filesystem::path &candidate,
                                  const std::filesystem::path &root) noexcept
{
    auto candidate_component = candidate.begin();
    for (const auto &root_component : root)
    {
        if (candidate_component == candidate.end() ||
            !components_equal(*candidate_component, root_component))
        {
            return false;
        }
        ++candidate_component;
    }
    return true;
}

/// @brief Reports whether two normalized Windows paths identify the same spelling-insensitive path.
[[nodiscard]] bool paths_equal(const std::filesystem::path &left,
                               const std::filesystem::path &right) noexcept
{
    return is_within_root(left, right) && is_within_root(right, left);
}

/// @brief Converts a Win32 timestamp into Unix-epoch system seconds.
[[nodiscard]] std::chrono::sys_seconds to_system_seconds(const LARGE_INTEGER value) noexcept
{
    constexpr std::int64_t windows_ticks_per_second = 10'000'000;
    constexpr std::int64_t windows_to_unix_epoch_seconds = 11'644'473'600;
    const auto unix_seconds =
        (value.QuadPart / windows_ticks_per_second) - windows_to_unix_epoch_seconds;
    return std::chrono::sys_seconds(std::chrono::seconds(unix_seconds));
}

/// @brief Classifies a target and preserves whether the original object was a reparse link.
[[nodiscard]] ConfinedEntryKind
classify_entry(const FILE_ATTRIBUTE_TAG_INFO &link_information,
               const FILE_ATTRIBUTE_TAG_INFO &target_information) noexcept
{
    if ((link_information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return ConfinedEntryKind::symbolic_link;
    }
    if ((target_information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return ConfinedEntryKind::directory;
    }
    if ((target_information.FileAttributes & FILE_ATTRIBUTE_DEVICE) == 0)
    {
        return ConfinedEntryKind::regular_file;
    }
    return ConfinedEntryKind::other;
}

} // namespace

/// @brief Holds the stable Windows parent handle and verified root path.
struct ConfinedDirectory::Implementation
{
    UniqueHandle directory;            ///< Parent used for every relative entry open.
    std::filesystem::path shared_root; ///< Root path resolved from its stable handle.
};

ConfinedDirectory::ConfinedDirectory(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

ConfinedDirectory::ConfinedDirectory(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory &ConfinedDirectory::operator=(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory::~ConfinedDirectory() = default;

Result<ConfinedDirectory, std::error_code>
open_confined_directory(const std::filesystem::path &shared_root,
                        const std::filesystem::path &directory)
{
    auto root_handle = open_path(shared_root, false);
    if (!root_handle)
    {
        return unexpected(root_handle.error());
    }
    auto stable_root = query_final_path(root_handle->get());
    if (!stable_root)
    {
        return unexpected(stable_root.error());
    }
    if (!paths_equal(stable_root.value(), shared_root.lexically_normal()))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto directory_handle = open_path(directory, false);
    if (!directory_handle)
    {
        return unexpected(directory_handle.error());
    }
    auto stable_directory = query_final_path(directory_handle->get());
    if (!stable_directory)
    {
        return unexpected(stable_directory.error());
    }
    if (!is_within_root(stable_directory.value(), stable_root.value()))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto implementation =
        std::make_unique<ConfinedDirectory::Implementation>(ConfinedDirectory::Implementation{
            std::move(directory_handle).value(), std::move(stable_root).value()});
    return ConfinedDirectory(std::move(implementation));
}

std::optional<ConfinedEntryMetadata>
read_confined_directory_entry(const ConfinedDirectory &directory,
                              const std::filesystem::path &native_name)
{
    if (native_name.empty() || native_name != native_name.filename())
    {
        return std::nullopt;
    }
    const auto &context = *directory.implementation_;
    auto link_handle = open_relative(context.directory.get(), native_name, true);
    auto target_handle = open_relative(context.directory.get(), native_name, false);
    if (!link_handle || !target_handle)
    {
        return std::optional<ConfinedEntryMetadata>{};
    }
    auto target_path = query_final_path(target_handle->get());
    if (!target_path || !is_within_root(target_path.value(), context.shared_root))
    {
        return std::optional<ConfinedEntryMetadata>{};
    }

    FILE_ATTRIBUTE_TAG_INFO link_information{};
    FILE_ATTRIBUTE_TAG_INFO target_information{};
    FILE_STANDARD_INFO standard_information{};
    FILE_BASIC_INFO basic_information{};
    if (GetFileInformationByHandleEx(link_handle->get(), FileAttributeTagInfo, &link_information,
                                     sizeof(link_information)) == FALSE ||
        GetFileInformationByHandleEx(target_handle->get(), FileAttributeTagInfo,
                                     &target_information, sizeof(target_information)) == FALSE ||
        GetFileInformationByHandleEx(target_handle->get(), FileStandardInfo, &standard_information,
                                     sizeof(standard_information)) == FALSE ||
        GetFileInformationByHandleEx(target_handle->get(), FileBasicInfo, &basic_information,
                                     sizeof(basic_information)) == FALSE)
    {
        return std::optional<ConfinedEntryMetadata>{};
    }

    const auto kind = classify_entry(link_information, target_information);
    std::optional<std::uintmax_t> size;
    if (kind == ConfinedEntryKind::regular_file)
    {
        if (standard_information.EndOfFile.QuadPart < 0)
        {
            return std::optional<ConfinedEntryMetadata>{};
        }
        size = static_cast<std::uintmax_t>(standard_information.EndOfFile.QuadPart);
    }
    return std::optional<ConfinedEntryMetadata>(std::in_place, kind, size,
                                                to_system_seconds(basic_information.LastWriteTime));
}

} // namespace sparenode::filesystem::detail

#endif
