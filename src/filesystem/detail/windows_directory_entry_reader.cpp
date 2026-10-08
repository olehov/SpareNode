#include "path_containment.hpp"
#include "sparenode/filesystem/detail/directory_entry_reader.hpp"
#include "windows_handle.hpp"
#include "windows_handle_path.hpp"

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Opens a filesystem object with optional final reparse traversal suppression.
[[nodiscard]] Result<UniqueWindowsHandle, std::error_code>
open_path(const std::filesystem::path &path, const bool open_reparse_point,
          const bool allow_delete_sharing = true, const bool require_oplock = false)
{
    auto sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (allow_delete_sharing)
    {
        sharing |= FILE_SHARE_DELETE;
    }
    auto flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (open_reparse_point)
    {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    if (require_oplock)
    {
        flags |= FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REQUIRING_OPLOCK;
    }
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return unexpected(last_windows_error());
    }
    return UniqueWindowsHandle(handle);
}

/// @brief Keeps one asynchronous directory oplock request alive.
struct DirectoryOplock
{
    UniqueWindowsHandle directory;         ///< Owns the handle carrying the pending request.
    REQUEST_OPLOCK_INPUT_BUFFER input{};   ///< Requested read-handle oplock level.
    REQUEST_OPLOCK_OUTPUT_BUFFER output{}; ///< Break details populated by the filesystem.
    OVERLAPPED operation{};                ///< Pending oplock request state.
    UniqueWindowsHandle event;             ///< Signals an oplock break.
    bool request_pending{};                ///< Whether the kernel still references this state.

    /// @brief Cancels and drains the asynchronous request before releasing its storage.
    ~DirectoryOplock() noexcept
    {
        if (!request_pending)
        {
            return;
        }
        static_cast<void>(CancelIoEx(directory.get(), &operation));
        DWORD transferred{};
        static_cast<void>(GetOverlappedResult(directory.get(), &operation, &transferred, TRUE));
    }

    /// @brief Borrows the directory handle protected by this oplock.
    [[nodiscard]] HANDLE native_handle() const noexcept
    {
        return directory.get();
    }
};

/// @brief Requests a read-handle oplock that blocks directory and ancestor relocation.
[[nodiscard]] Result<std::unique_ptr<DirectoryOplock>, std::error_code>
request_directory_oplock(UniqueWindowsHandle directory)
{
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
    {
        return unexpected(last_windows_error());
    }
    auto oplock = std::make_unique<DirectoryOplock>();
    oplock->directory = std::move(directory);
    oplock->event = UniqueWindowsHandle(event);
    oplock->input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    oplock->input.StructureLength = sizeof(oplock->input);
    oplock->input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE;
    oplock->input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    oplock->output.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    oplock->output.StructureLength = sizeof(oplock->output);
    oplock->operation.hEvent = oplock->event.get();
    const auto requested = DeviceIoControl(oplock->directory.get(), FSCTL_REQUEST_OPLOCK,
                                           &oplock->input, sizeof(oplock->input), &oplock->output,
                                           sizeof(oplock->output), nullptr, &oplock->operation);
    if (requested == FALSE && GetLastError() == ERROR_IO_PENDING)
    {
        oplock->request_pending = true;
        return oplock;
    }
    return unexpected(requested == FALSE
                          ? last_windows_error()
                          : std::make_error_code(std::errc::operation_not_supported));
}

/// @brief Opens one basename relative to a retained directory handle.
[[nodiscard]] Result<UniqueWindowsHandle, std::error_code>
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
    return UniqueWindowsHandle(handle);
}

/// @brief Reports whether two normalized Windows paths identify the same spelling-insensitive path.
[[nodiscard]] bool paths_equal(const std::filesystem::path &left,
                               const std::filesystem::path &right) noexcept
{
    return is_within_root(left, PathBoundary{right}, windows_path_components_equal) &&
           is_within_root(right, PathBoundary{left}, windows_path_components_equal);
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
    std::unique_ptr<DirectoryOplock> oplock; ///< Optional relocation barrier.
    UniqueWindowsHandle directory;           ///< Parent when no relocation barrier is required.
    std::filesystem::path shared_root;       ///< Root path resolved from its stable handle.

    /// @brief Borrows the parent handle from its applicable owner.
    [[nodiscard]] HANDLE native_handle() const noexcept
    {
        return oplock ? oplock->native_handle() : directory.get();
    }
};

ConfinedDirectory::ConfinedDirectory(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

ConfinedDirectory::ConfinedDirectory(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory &ConfinedDirectory::operator=(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory::~ConfinedDirectory() = default;

/// @brief Returns the retained parent handle for relative operations.
void *ConfinedDirectory::native_handle() const noexcept
{
    return implementation_->native_handle();
}

/// @brief Opens a Windows directory whose final handle path remains inside the shared root.
Result<ConfinedDirectory, std::error_code>
open_confined_directory(const std::filesystem::path &shared_root,
                        const std::filesystem::path &directory)
{
    auto root_handle = open_path(shared_root, false);
    if (!root_handle)
    {
        return unexpected(root_handle.error());
    }
    auto stable_root = query_final_windows_path(root_handle->get());
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
    auto stable_directory = query_final_windows_path(directory_handle->get());
    if (!stable_directory)
    {
        return unexpected(stable_directory.error());
    }
    if (!is_within_root(stable_directory.value(), PathBoundary{stable_root.value()},
                        windows_path_components_equal))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto implementation =
        std::make_unique<ConfinedDirectory::Implementation>(ConfinedDirectory::Implementation{
            nullptr, std::move(directory_handle).value(), std::move(stable_root).value()});
    return ConfinedDirectory(std::move(implementation));
}

/// @brief Opens a Windows mutation parent under an oplock that blocks ancestor relocation.
Result<ConfinedDirectory, std::error_code>
open_confined_mutation_directory(const std::filesystem::path &shared_root,
                                 const std::filesystem::path &directory)
{
    auto directory_handle = open_path(directory, false, true, true);
    if (!directory_handle)
    {
        return unexpected(directory_handle.error());
    }
    auto oplock = request_directory_oplock(std::move(directory_handle).value());
    if (!oplock)
    {
        return unexpected(oplock.error());
    }

    auto root_handle = open_path(shared_root, false);
    if (!root_handle)
    {
        return unexpected(root_handle.error());
    }
    auto stable_root = query_final_windows_path(root_handle->get());
    if (!stable_root)
    {
        return unexpected(stable_root.error());
    }
    if (!paths_equal(stable_root.value(), shared_root.lexically_normal()))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto stable_directory = query_final_windows_path(oplock.value()->native_handle());
    if (!stable_directory)
    {
        return unexpected(stable_directory.error());
    }
    if (!is_within_root(stable_directory.value(), PathBoundary{stable_root.value()},
                        windows_path_components_equal))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto implementation =
        std::make_unique<ConfinedDirectory::Implementation>(ConfinedDirectory::Implementation{
            std::move(oplock).value(), {}, std::move(stable_root).value()});
    return ConfinedDirectory(std::move(implementation));
}

/// @brief Reads metadata through the already-confined Windows directory handle.
std::optional<ConfinedEntryMetadata>
read_confined_directory_entry(const ConfinedDirectory &directory,
                              const std::filesystem::path &native_name)
{
    if (native_name.empty() || native_name != native_name.filename())
    {
        return std::nullopt;
    }
    const auto &context = *directory.implementation_;
    auto link_handle = open_relative(context.native_handle(), native_name, true);
    auto target_handle = open_relative(context.native_handle(), native_name, false);
    if (!link_handle || !target_handle)
    {
        return std::optional<ConfinedEntryMetadata>{};
    }
    auto target_path = query_final_windows_path(target_handle->get());
    if (!target_path || !is_within_root(target_path.value(), PathBoundary{context.shared_root},
                                        windows_path_components_equal))
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
