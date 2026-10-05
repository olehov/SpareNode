#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include "temporary_name.hpp"
#include "windows_handle.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// One bounded copy buffer; stage creation and rename remain handle-relative.
constexpr std::size_t copy_buffer_bytes = 64 * 1024;
constexpr std::size_t maximum_stage_attempts = 100;

/// @brief Removes an unpublished sibling by handle, avoiding path re-resolution.
struct StageCleanup
{
    HANDLE handle{};  ///< Open staging file with DELETE access.
    bool published{}; ///< Rename moved the stage into its final name.

    ~StageCleanup()
    {
        if (!published)
        {
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                                         sizeof(disposition)));
        }
    }
};

/// @brief Converts one Windows or NT failure into the finalization contract.
[[nodiscard]] UploadFinalizationError failure(const UploadFinalizationErrorCode code,
                                              const DWORD native_error) noexcept
{
    return {code, {}, {static_cast<int>(native_error), std::system_category()}};
}

/// @brief Gives cancellation priority over an expired deadline.
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

/// @brief Exclusively creates a sibling from the retained directory handle.
[[nodiscard]] Result<UniqueWindowsHandle, UploadFinalizationError> create_stage(const HANDLE parent)
{
    for (std::size_t attempt = 0; attempt < maximum_stage_attempts; ++attempt)
    {
        const auto name =
            std::filesystem::path(".sparenode-upload-" + temporary_name_suffix() + ".tmp").native();
        const auto name_bytes = name.size() * sizeof(wchar_t);
        if (name_bytes > (std::numeric_limits<USHORT>::max)())
        {
            return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::staging_failed});
        }
        UNICODE_STRING native_name{};
        native_name.Length = static_cast<USHORT>(name_bytes);
        native_name.MaximumLength = native_name.Length;
        native_name.Buffer = const_cast<PWSTR>(name.data());
        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, &native_name, OBJ_CASE_INSENSITIVE, parent,
                                   nullptr);
        IO_STATUS_BLOCK status_block{};
        HANDLE stage = INVALID_HANDLE_VALUE;
        const auto status = NtCreateFile(
            &stage, GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE, &attributes,
            &status_block, nullptr, FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN,
            FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_CREATE,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
        if (status >= 0)
        {
            return UniqueWindowsHandle(stage);
        }
        const auto native_error = RtlNtStatusToDosError(status);
        if (native_error != ERROR_FILE_EXISTS && native_error != ERROR_ALREADY_EXISTS)
        {
            return unexpected(failure(UploadFinalizationErrorCode::staging_failed, native_error));
        }
    }
    return unexpected(failure(UploadFinalizationErrorCode::staging_failed, ERROR_FILE_EXISTS));
}

/// @brief Copies only the validated source length to an unpublished sibling.
[[nodiscard]] Result<void, UploadFinalizationError>
copy_source(const HANDLE source, const HANDLE stage, const std::uint64_t expected_bytes,
            const TemporaryFileIoOptions &options)
{
    std::array<std::byte, copy_buffer_bytes> buffer{};
    std::uint64_t copied = 0;
    while (copied < expected_bytes)
    {
        if (const auto stopped = interruption(options); stopped)
        {
            return unexpected(stopped.value());
        }
        const auto requested = static_cast<DWORD>(
            (std::min)(expected_bytes - copied, static_cast<std::uint64_t>(buffer.size())));
        DWORD received = 0;
        if (ReadFile(source, buffer.data(), requested, &received, nullptr) == FALSE)
        {
            return unexpected(
                failure(UploadFinalizationErrorCode::source_read_failed, GetLastError()));
        }
        if (received == 0)
        {
            return unexpected(
                UploadFinalizationError{UploadFinalizationErrorCode::source_read_failed});
        }
        DWORD offset = 0;
        while (offset < received)
        {
            if (const auto stopped = interruption(options); stopped)
            {
                return unexpected(stopped.value());
            }
            DWORD written = 0;
            if (WriteFile(stage, buffer.data() + offset, received - offset, &written, nullptr) ==
                FALSE)
            {
                return unexpected(
                    failure(UploadFinalizationErrorCode::staging_write_failed, GetLastError()));
            }
            if (written == 0)
            {
                return unexpected(
                    UploadFinalizationError{UploadFinalizationErrorCode::staging_write_failed});
            }
            offset += written;
        }
        copied += received;
    }
    return {};
}

/// @brief Renames the open sibling relative to its retained parent without replacement.
[[nodiscard]] Result<void, UploadFinalizationError>
publish_stage(const HANDLE stage, const HANDLE parent, const std::filesystem::path &native_name)
{
    const auto &name = native_name.native();
    const auto name_bytes = name.size() * sizeof(wchar_t);
    if (name_bytes > (std::numeric_limits<ULONG>::max)())
    {
        return unexpected(
            UploadFinalizationError{UploadFinalizationErrorCode::invalid_destination});
    }
    const auto structure_bytes = offsetof(FILE_RENAME_INFO, FileName) + name_bytes;
    std::unique_ptr<void, decltype(&std::free)> storage(std::malloc(structure_bytes), &std::free);
    if (!storage)
    {
        return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::resource_failure});
    }
    std::memset(storage.get(), 0, structure_bytes);
    auto *rename = static_cast<FILE_RENAME_INFO *>(storage.get());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = parent;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, name.data(), name_bytes);

    using NtSetInformationFileFunction =
        NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    constexpr ULONG file_rename_information_class = 10;
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto native_rename = module == nullptr
                                   ? nullptr
                                   : reinterpret_cast<NtSetInformationFileFunction>(
                                         GetProcAddress(module, "NtSetInformationFile"));
    if (native_rename == nullptr)
    {
        return unexpected(
            failure(UploadFinalizationErrorCode::publish_failed, ERROR_PROC_NOT_FOUND));
    }
    IO_STATUS_BLOCK status_block{};
    const auto status =
        native_rename(stage, &status_block, rename, static_cast<ULONG>(structure_bytes),
                      file_rename_information_class);
    if (status >= 0)
    {
        return {};
    }
    const auto native_error = RtlNtStatusToDosError(status);
    return unexpected(
        failure(native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS
                    ? UploadFinalizationErrorCode::destination_exists
                    : UploadFinalizationErrorCode::publish_failed,
                native_error));
}

} // namespace

/// @brief Copies to a handle-relative sibling and atomically renames without overwrite.
Result<void, UploadFinalizationError>
publish_confined_upload(const ConfinedDirectory &directory, const TemporaryFile &source,
                        const std::filesystem::path &native_name,
                        const TemporaryFileIoOptions &options)
{
    constexpr DWORD source_flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN;
    const auto source_handle =
        CreateFileW(source.path().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, source_flags, nullptr);
    if (source_handle == INVALID_HANDLE_VALUE)
    {
        return unexpected(failure(UploadFinalizationErrorCode::source_read_failed, GetLastError()));
    }
    UniqueWindowsHandle source_file(source_handle);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER source_size{};
    if (GetFileInformationByHandleEx(source_file.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        GetFileSizeEx(source_file.get(), &source_size) == FALSE || source_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(source_size.QuadPart) != source.size())
    {
        return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::source_read_failed});
    }
    auto created = create_stage(directory.native_handle());
    if (!created)
    {
        return unexpected(created.error());
    }
    auto stage = std::move(created).value();
    StageCleanup cleanup{stage.get()};
    if (auto copied = copy_source(source_file.get(), stage.get(), source.size(), options); !copied)
    {
        return unexpected(copied.error());
    }
    if (const auto stopped = interruption(options); stopped)
    {
        return unexpected(stopped.value());
    }
    if (FlushFileBuffers(stage.get()) == FALSE)
    {
        return unexpected(
            failure(UploadFinalizationErrorCode::staging_flush_failed, GetLastError()));
    }
    FILE_BASIC_INFO basic_information{};
    if (GetFileInformationByHandleEx(stage.get(), FileBasicInfo, &basic_information,
                                     sizeof(basic_information)) == FALSE)
    {
        return unexpected(failure(UploadFinalizationErrorCode::staging_failed, GetLastError()));
    }
    basic_information.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (SetFileInformationByHandle(stage.get(), FileBasicInfo, &basic_information,
                                   sizeof(basic_information)) == FALSE)
    {
        return unexpected(failure(UploadFinalizationErrorCode::staging_failed, GetLastError()));
    }
    if (const auto stopped = interruption(options); stopped)
    {
        return unexpected(stopped.value());
    }
    if (auto published = publish_stage(stage.get(), directory.native_handle(), native_name);
        !published)
    {
        return unexpected(published.error());
    }
    cleanup.published = true;
    return {};
}

} // namespace sparenode::filesystem::detail

#endif
