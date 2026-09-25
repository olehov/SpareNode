#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Owns one POSIX file descriptor.
class FileDescriptor final
{
  public:
    /// @brief Takes ownership of a descriptor returned by open or openat.
    explicit FileDescriptor(const int descriptor) noexcept : descriptor_(descriptor)
    {
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    /// @brief Transfers descriptor ownership.
    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }

    FileDescriptor &operator=(FileDescriptor &&) = delete;

    /// @brief Closes the owned descriptor.
    ~FileDescriptor()
    {
        if (descriptor_ >= 0)
        {
            static_cast<void>(::close(descriptor_));
        }
    }

    /// @brief Returns the descriptor without releasing ownership.
    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    int descriptor_{-1}; ///< Owned descriptor, or -1 after ownership transfer.
};

/// @brief Captures the current errno value as a portable error code.
[[nodiscard]] std::error_code last_system_error() noexcept
{
    return {errno, std::generic_category()};
}

/// @brief Gives a root path a distinct type at containment call sites.
struct PathBoundary
{
    const std::filesystem::path &path; ///< Canonical root required as a complete prefix.
};

/// @brief Checks component-wise containment for canonical absolute paths.
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

/// @brief Resolves the stable object currently owned by a Linux descriptor.
[[nodiscard]] Result<std::filesystem::path, std::error_code>
descriptor_path(const FileDescriptor &descriptor)
{
    const auto proc_path =
        std::filesystem::path("/proc/self/fd") / std::to_string(descriptor.get());
    std::error_code error;
    auto path = std::filesystem::canonical(proc_path, error);
    if (error)
    {
        return unexpected(error);
    }
    return path.lexically_normal();
}

/// @brief Opens a stable directory descriptor without following its final component.
[[nodiscard]] Result<FileDescriptor, std::error_code>
open_directory(const std::filesystem::path &path)
{
    const auto descriptor = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
    {
        return unexpected(last_system_error());
    }
    return FileDescriptor(descriptor);
}

/// @brief Converts Linux stat mode bits into a portable confined entry kind.
[[nodiscard]] ConfinedEntryKind classify_entry(const struct stat &link_information,
                                               const struct stat &target_information) noexcept
{
    if (S_ISLNK(link_information.st_mode))
    {
        return ConfinedEntryKind::symbolic_link;
    }
    if (S_ISREG(target_information.st_mode))
    {
        return ConfinedEntryKind::regular_file;
    }
    if (S_ISDIR(target_information.st_mode))
    {
        return ConfinedEntryKind::directory;
    }
    return ConfinedEntryKind::other;
}

/// @brief Holds a stable parent descriptor and the verified canonical root path.
struct StableDirectoryContext
{
    FileDescriptor directory;          ///< Parent used for every relative entry open.
    std::filesystem::path shared_root; ///< Root path resolved from its stable descriptor.
};

/// @brief Opens and verifies the root and parent directory before per-entry inspection.
[[nodiscard]] Result<StableDirectoryContext, std::error_code>
open_directory_context(const ConfinedDirectoryEntryRequest &request)
{
    auto root_descriptor = open_directory(request.shared_root);
    if (!root_descriptor)
    {
        return unexpected(root_descriptor.error());
    }
    auto stable_root = descriptor_path(root_descriptor.value());
    if (!stable_root)
    {
        return unexpected(stable_root.error());
    }
    if (stable_root.value() != request.shared_root.lexically_normal())
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto directory_descriptor = open_directory(request.directory);
    if (!directory_descriptor)
    {
        return unexpected(directory_descriptor.error());
    }
    auto stable_directory = descriptor_path(directory_descriptor.value());
    if (!stable_directory)
    {
        return unexpected(stable_directory.error());
    }
    if (!is_within_root(stable_directory.value(), PathBoundary{stable_root.value()}))
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    return StableDirectoryContext{std::move(directory_descriptor).value(),
                                  std::move(stable_root).value()};
}

/// @brief Opens one child relative to a stable parent and reads only handle-bound metadata.
[[nodiscard]] std::optional<ConfinedEntryMetadata>
read_entry_metadata(const StableDirectoryContext &context, const std::filesystem::path &native_name)
{
    const auto link_descriptor =
        ::openat(context.directory.get(), native_name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (link_descriptor < 0)
    {
        return std::nullopt;
    }
    const FileDescriptor stable_link(link_descriptor);

    const auto target_descriptor =
        ::openat(context.directory.get(), native_name.c_str(), O_PATH | O_CLOEXEC);
    if (target_descriptor < 0)
    {
        return std::nullopt;
    }
    const FileDescriptor stable_target(target_descriptor);
    auto target_path = descriptor_path(stable_target);
    if (!target_path || !is_within_root(target_path.value(), PathBoundary{context.shared_root}))
    {
        return std::nullopt;
    }

    struct stat link_information
    {
    };
    struct stat target_information
    {
    };
    if (::fstat(stable_link.get(), &link_information) != 0 ||
        ::fstat(stable_target.get(), &target_information) != 0)
    {
        return std::nullopt;
    }

    const auto kind = classify_entry(link_information, target_information);
    std::optional<std::uintmax_t> size;
    if (kind == ConfinedEntryKind::regular_file)
    {
        if (target_information.st_size < 0)
        {
            return std::nullopt;
        }
        size = static_cast<std::uintmax_t>(target_information.st_size);
    }
    const auto modification_time =
        std::chrono::sys_seconds(std::chrono::seconds(target_information.st_mtim.tv_sec));
    return ConfinedEntryMetadata{kind, size, modification_time};
}

} // namespace

Result<std::optional<ConfinedEntryMetadata>, std::error_code>
read_confined_directory_entry(const ConfinedDirectoryEntryRequest &request)
{
    if (request.native_name.empty() || request.native_name != request.native_name.filename())
    {
        return std::optional<ConfinedEntryMetadata>{};
    }
    auto context = open_directory_context(request);
    if (!context)
    {
        return unexpected(context.error());
    }
    return read_entry_metadata(context.value(), request.native_name);
}

} // namespace sparenode::filesystem::detail

#endif
