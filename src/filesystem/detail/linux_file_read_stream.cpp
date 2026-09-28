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

/// @brief Owns one POSIX file descriptor.
class FileDescriptor final
{
  public:
    explicit FileDescriptor(const int descriptor) noexcept : descriptor_(descriptor)
    {
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }
    FileDescriptor &operator=(FileDescriptor &&) = delete;
    ~FileDescriptor()
    {
        if (descriptor_ >= 0)
        {
            static_cast<void>(::close(descriptor_));
        }
    }
    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    int descriptor_{-1};
};

[[nodiscard]] std::error_code last_system_error() noexcept
{
    return {errno, std::generic_category()};
}

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

[[nodiscard]] Result<FileDescriptor, std::error_code>
open_descriptor(const std::filesystem::path &path, const int flags)
{
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0)
    {
        return unexpected(last_system_error());
    }
    return FileDescriptor(descriptor);
}

[[nodiscard]] Result<std::filesystem::path, std::error_code>
descriptor_path(const FileDescriptor &descriptor)
{
    const auto proc_path =
        std::filesystem::path("/proc/self/fd") / std::to_string(descriptor.get());
    std::error_code error;
    auto path = std::filesystem::read_symlink(proc_path, error);
    if (error)
    {
        return unexpected(error);
    }
    if (!path.is_absolute())
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    return path.lexically_normal();
}

struct PathBoundary
{
    const std::filesystem::path &path; ///< Root required as a complete component prefix.
};

[[nodiscard]] bool is_within_root(const std::filesystem::path &candidate,
                                  const PathBoundary root) noexcept
{
    auto candidate_component = candidate.begin();
    for (const auto &root_component : root.path)
    {
        if (candidate_component == candidate.end() || *candidate_component != root_component)
        {
            return false;
        }
        ++candidate_component;
    }
    return true;
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

        auto file = open_descriptor(safe_path->path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (!file)
        {
            return unexpected(system_failure(file.error()));
        }
        auto stable_file = descriptor_path(file.value());
        if (!stable_file)
        {
            return unexpected(system_failure(stable_file.error()));
        }
        if (!is_within_root(stable_file.value(), PathBoundary{stable_root.value()}))
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
            return unexpected(system_failure(last_system_error()));
        }
        if (!S_ISREG(information.st_mode) || information.st_size < 0)
        {
            return unexpected(FileReadError{FileReadErrorCode::not_regular_file, {}, std::nullopt});
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
        const auto error = last_system_error();
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
