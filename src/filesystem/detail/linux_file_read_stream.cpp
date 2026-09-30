#include "path_containment.hpp"
#include "posix_file_descriptor.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace sparenode::filesystem
{
namespace
{

using detail::descriptor_path;
using detail::FileDescriptor;
using detail::last_posix_error;

/// @brief Maps a native POSIX failure into the file-read error domain.
[[nodiscard]] FileReadError system_failure(const std::error_code error) noexcept
{
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
    {
        return {FileReadErrorCode::not_found, error, std::nullopt};
    }
    if (error == std::errc::permission_denied)
    {
        return {FileReadErrorCode::permission_denied, error, std::nullopt};
    }
    return {FileReadErrorCode::filesystem_failure, error, std::nullopt};
}

/// @brief Opens one POSIX path with close-on-exec and the requested access flags.
[[nodiscard]] Result<FileDescriptor, std::error_code>
open_descriptor(const std::filesystem::path &path, const int flags)
{
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0)
    {
        return unexpected(last_posix_error());
    }
    return FileDescriptor(descriptor);
}

} // namespace

/// @brief Retains the stable Linux descriptor and captured file length.
struct FileReadStream::Implementation
{
    FileDescriptor descriptor; ///< Descriptor used for every incremental read.
    std::uint64_t size{};      ///< Length captured with fstat after opening.
};

FileReadStream::FileReadStream(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

FileReadStream::FileReadStream(FileReadStream &&) noexcept = default;
FileReadStream &FileReadStream::operator=(FileReadStream &&) noexcept = default;
FileReadStream::~FileReadStream() = default;

/// @brief Opens a regular POSIX file and verifies its post-open containment.
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

        auto root =
            open_descriptor(shared_root.path(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (!root)
        {
            return unexpected(system_failure(root.error()));
        }
        auto stable_root = descriptor_path(root.value());
        if (!stable_root)
        {
            return unexpected(system_failure(stable_root.error()));
        }

        auto file =
            open_descriptor(safe_path->path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (!file)
        {
            return unexpected(system_failure(file.error()));
        }
        auto stable_file = descriptor_path(file.value());
        if (!stable_file)
        {
            return unexpected(system_failure(stable_file.error()));
        }
        if (!detail::is_within_root(stable_file.value(), detail::PathBoundary{stable_root.value()}))
        {
            return unexpected(FileReadError{FileReadErrorCode::outside_shared_root,
                                            {},
                                            SafePathErrorCode::outside_shared_root});
        }

        struct stat information
        {
        };
        if (::fstat(file->get(), &information) != 0)
        {
            return unexpected(system_failure(last_posix_error()));
        }
        if (!S_ISREG(information.st_mode) || information.st_size < 0)
        {
            return unexpected(FileReadError{FileReadErrorCode::not_regular_file, {}, std::nullopt});
        }
        const int status_flags = ::fcntl(file->get(), F_GETFL);
        if (status_flags < 0)
        {
            return unexpected(system_failure(last_posix_error()));
        }
        if (::fcntl(file->get(), F_SETFL, status_flags & ~O_NONBLOCK) != 0)
        {
            return unexpected(system_failure(last_posix_error()));
        }
        auto implementation = std::make_unique<Implementation>(Implementation{
            std::move(file).value(), static_cast<std::uint64_t>(information.st_size)});
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

/// @brief Reads the next bounded block from the owned POSIX descriptor.
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
    while (true)
    {
        const auto count =
            ::read(implementation_->descriptor.get(), destination.data(), destination.size());
        if (count >= 0)
        {
            return static_cast<std::size_t>(count);
        }
        const auto error = last_posix_error();
        if (error != std::errc::interrupted)
        {
            return unexpected(system_failure(error));
        }
        if (stop_token.stop_requested())
        {
            return unexpected(FileReadError{FileReadErrorCode::cancelled, {}, std::nullopt});
        }
    }
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
