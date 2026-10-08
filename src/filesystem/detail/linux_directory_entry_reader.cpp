#include "path_containment.hpp"
#include "posix_file_descriptor.hpp"
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

/// @brief Opens a stable directory descriptor without following its final component.
[[nodiscard]] Result<FileDescriptor, std::error_code>
open_directory(const std::filesystem::path &path)
{
    const auto descriptor = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
    {
        return unexpected(last_posix_error());
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

} // namespace

/// @brief Holds the stable Linux parent descriptor and verified root path.
struct ConfinedDirectory::Implementation
{
    FileDescriptor root;               ///< Root used to constrain destructive operations.
    FileDescriptor directory;          ///< Parent used for every relative entry open.
    std::filesystem::path shared_root; ///< Root path resolved from its stable descriptor.
};

ConfinedDirectory::ConfinedDirectory(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

ConfinedDirectory::ConfinedDirectory(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory &ConfinedDirectory::operator=(ConfinedDirectory &&) noexcept = default;
ConfinedDirectory::~ConfinedDirectory() = default;

/// @brief Returns the retained parent descriptor for relative operations.
int ConfinedDirectory::native_handle() const noexcept
{
    return implementation_->directory.get();
}

/// @brief Returns the retained shared-root descriptor for kernel confinement.
int ConfinedDirectory::root_native_handle() const noexcept
{
    return implementation_->root.get();
}

/// @brief Opens a POSIX directory whose descriptor path remains inside the shared root.
Result<ConfinedDirectory, std::error_code>
open_confined_directory(const std::filesystem::path &shared_root,
                        const std::filesystem::path &directory)
{
    auto root_descriptor = open_directory(shared_root);
    if (!root_descriptor)
    {
        return unexpected(root_descriptor.error());
    }
    auto stable_root = descriptor_path(root_descriptor.value());
    if (!stable_root)
    {
        return unexpected(stable_root.error());
    }
    if (stable_root.value() != shared_root.lexically_normal())
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto directory_descriptor = open_directory(directory);
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
    auto implementation =
        std::make_unique<ConfinedDirectory::Implementation>(ConfinedDirectory::Implementation{
            std::move(root_descriptor).value(), std::move(directory_descriptor).value(),
            std::move(stable_root).value()});
    return ConfinedDirectory(std::move(implementation));
}

/// @brief Opens a Linux mutation context whose root descriptor can anchor Landlock.
Result<ConfinedDirectory, std::error_code>
open_confined_mutation_directory(const std::filesystem::path &shared_root,
                                 const std::filesystem::path &directory)
{
    return open_confined_directory(shared_root, directory);
}

/// @brief Opens one child relative to a stable parent and reads only handle-bound metadata.
std::optional<ConfinedEntryMetadata>
read_confined_directory_entry(const ConfinedDirectory &directory,
                              const std::filesystem::path &native_name)
{
    if (native_name.empty() || native_name != native_name.filename())
    {
        return std::nullopt;
    }
    const auto &context = *directory.implementation_;
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

} // namespace sparenode::filesystem::detail

#endif
