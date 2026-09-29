#include "path_containment.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "windows_handle.hpp"
#include "windows_handle_path.hpp"

#ifdef _WIN32

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sparenode::filesystem
{
namespace
{

[[nodiscard]] FileReadError system_failure(const std::error_code error) noexcept
{
    if (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND ||
        error.value() == ERROR_INVALID_NAME)
    {
        return {FileReadErrorCode::not_found, error, std::nullopt};
    }
    if (error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION)
    {
        return {FileReadErrorCode::permission_denied, error, std::nullopt};
    }
    return {FileReadErrorCode::filesystem_failure, error, std::nullopt};
}

[[nodiscard]] Result<detail::UniqueWindowsHandle, std::error_code>
open_path(const std::filesystem::path &path, const DWORD access, const DWORD flags)
{
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const auto handle =
        CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return unexpected(detail::last_windows_error());
    }
    return detail::UniqueWindowsHandle(handle);
}

} // namespace

/// @brief Retains the stable Win32 handle and captured file length.
struct FileReadStream::Implementation
{
    detail::UniqueWindowsHandle handle; ///< Handle used for every incremental read.
    std::uint64_t size{};               ///< Length captured from the opened handle.
};

FileReadStream::FileReadStream(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

FileReadStream::FileReadStream(FileReadStream &&) noexcept = default;
FileReadStream &FileReadStream::operator=(FileReadStream &&) noexcept = default;
FileReadStream::~FileReadStream() = default;

Result<FileReadStream, FileReadError>
FileReadStream::open(const configuration::SharedRoot &shared_root,
                     const std::string_view requested_path)
{
    try
    {
        auto safe_path = SafePath::resolve(shared_root, requested_path);
        if (!safe_path)
        {
            return unexpected(
                FileReadError{FileReadErrorCode::invalid_path, {}, safe_path.error().code});
        }

        auto root = open_path(shared_root.path(), FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
        if (!root)
        {
            return unexpected(system_failure(root.error()));
        }
        auto stable_root = detail::query_final_windows_path(root->get());
        if (!stable_root)
        {
            return unexpected(system_failure(stable_root.error()));
        }

        auto file = open_path(safe_path->path(), GENERIC_READ,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN);
        if (!file)
        {
            return unexpected(system_failure(file.error()));
        }
        auto stable_file = detail::query_final_windows_path(file->get());
        if (!stable_file)
        {
            return unexpected(system_failure(stable_file.error()));
        }
        if (!detail::is_within_root(stable_file.value(), detail::PathBoundary{stable_root.value()},
                                    detail::windows_path_components_equal))
        {
            return unexpected(FileReadError{FileReadErrorCode::outside_shared_root,
                                            {},
                                            SafePathErrorCode::outside_shared_root});
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO information{};
        if (GetFileInformationByHandleEx(file->get(), FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE ||
            GetFileInformationByHandleEx(file->get(), FileStandardInfo, &information,
                                         sizeof(information)) == FALSE)
        {
            return unexpected(system_failure(detail::last_windows_error()));
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            GetFileType(file->get()) != FILE_TYPE_DISK || information.EndOfFile.QuadPart < 0)
        {
            return unexpected(FileReadError{FileReadErrorCode::not_regular_file, {}, std::nullopt});
        }

        auto implementation = std::make_unique<Implementation>(Implementation{
            std::move(file).value(), static_cast<std::uint64_t>(information.EndOfFile.QuadPart)});
        return FileReadStream(std::move(implementation));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            FileReadError{FileReadErrorCode::resource_allocation_failed, {}, std::nullopt});
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        return unexpected(system_failure(error.code()));
    }
}

std::uint64_t FileReadStream::size() const noexcept
{
    return implementation_->size;
}

Result<std::size_t, FileReadError> FileReadStream::read(const std::span<std::byte> destination,
                                                        const std::stop_token &stop_token)
{
    if (stop_token.stop_requested())
    {
        return unexpected(FileReadError{FileReadErrorCode::cancelled, {}, std::nullopt});
    }
    if (destination.empty())
    {
        return std::size_t{0};
    }
    const auto requested = static_cast<DWORD>((std::min)(
        destination.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD count = 0;
    if (ReadFile(implementation_->handle.get(), destination.data(), requested, &count, nullptr) ==
        FALSE)
    {
        return unexpected(system_failure(detail::last_windows_error()));
    }
    return static_cast<std::size_t>(count);
}

const char *to_string(const FileReadErrorCode code) noexcept
{
    switch (code)
    {
    case FileReadErrorCode::invalid_path:
        return "file path is invalid";
    case FileReadErrorCode::not_found:
        return "file does not exist";
    case FileReadErrorCode::not_regular_file:
        return "requested path is not a regular file";
    case FileReadErrorCode::permission_denied:
        return "file access was denied";
    case FileReadErrorCode::outside_shared_root:
        return "file is outside the shared root";
    case FileReadErrorCode::filesystem_failure:
        return "file read failed";
    case FileReadErrorCode::cancelled:
        return "file read was cancelled";
    case FileReadErrorCode::resource_allocation_failed:
        return "file stream memory allocation failed";
    }
    return "unknown file read error";
}

} // namespace sparenode::filesystem

#endif
