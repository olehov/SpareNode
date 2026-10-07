#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

#ifndef _WIN32

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "posix_file_descriptor.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Converts the current errno into the portable deletion contract.
[[nodiscard]] FilesystemEntryRemovalError failure(const int native_error) noexcept
{
    auto code = FilesystemEntryRemovalErrorCode::removal_failed;
    if (native_error == ENOENT || native_error == ENOTDIR)
    {
        code = FilesystemEntryRemovalErrorCode::not_found;
    }
    else if (native_error == ENOTEMPTY || native_error == EEXIST)
    {
        code = FilesystemEntryRemovalErrorCode::directory_not_empty;
    }
    else if (native_error == EACCES || native_error == EPERM || native_error == EROFS)
    {
        code = FilesystemEntryRemovalErrorCode::permission_denied;
    }
    return {code, {}, {native_error, std::generic_category()}};
}

/// @brief Restricts the calling thread to removing entries below one retained root.
[[nodiscard]] std::error_code apply_deletion_landlock(const int root_descriptor) noexcept
{
    constexpr std::uint64_t removal_access =
        LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE;
    landlock_ruleset_attr ruleset_attributes{};
    ruleset_attributes.handled_access_fs = removal_access;
    const auto ruleset = static_cast<int>(::syscall(
        __NR_landlock_create_ruleset, &ruleset_attributes, sizeof(ruleset_attributes), 0));
    if (ruleset < 0)
    {
        return last_posix_error();
    }
    const FileDescriptor ruleset_descriptor(ruleset);

    landlock_path_beneath_attr root_rule{};
    root_rule.allowed_access = removal_access;
    root_rule.parent_fd = root_descriptor;
    if (::syscall(__NR_landlock_add_rule, ruleset_descriptor.get(), LANDLOCK_RULE_PATH_BENEATH,
                  &root_rule, 0) != 0)
    {
        return last_posix_error();
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        return last_posix_error();
    }
    if (::syscall(__NR_landlock_restrict_self, ruleset_descriptor.get(), 0) != 0)
    {
        return last_posix_error();
    }
    return {};
}

/// @brief Carries the result from the one-operation Landlock thread.
struct RemovalOutcome
{
    bool removed{}; ///< Whether the entry was deleted.
    FilesystemEntryRemovalError error{
        FilesystemEntryRemovalErrorCode::removal_failed}; ///< Failure when not removed.
};

/// @brief Performs one deletion after entering a root-scoped Landlock domain.
void remove_in_landlock(const ConfinedDirectory &parent, const std::filesystem::path &native_name,
                        RemovalOutcome &outcome) noexcept
{
    if (const auto confinement_error = apply_deletion_landlock(parent.root_native_handle());
        confinement_error)
    {
        outcome.error = {FilesystemEntryRemovalErrorCode::removal_failed, {}, confinement_error};
        return;
    }
    if (::unlinkat(parent.native_handle(), native_name.c_str(), 0) == 0)
    {
        outcome.removed = true;
        return;
    }
    const auto file_error = errno;
    if (file_error == EISDIR &&
        ::unlinkat(parent.native_handle(), native_name.c_str(), AT_REMOVEDIR) == 0)
    {
        outcome.removed = true;
        return;
    }
    outcome.error = failure(file_error == EISDIR ? errno : file_error);
}

} // namespace

Result<void, FilesystemEntryRemovalError>
remove_confined_entry(const ConfinedDirectory &parent, const std::filesystem::path &native_name)
{
    if (native_name.empty() || native_name != native_name.filename())
    {
        return unexpected(
            FilesystemEntryRemovalError{FilesystemEntryRemovalErrorCode::invalid_destination});
    }
    RemovalOutcome outcome;
    std::thread worker(remove_in_landlock, std::cref(parent), std::cref(native_name),
                       std::ref(outcome));
    worker.join();
    if (outcome.removed)
    {
        return {};
    }
    return unexpected(outcome.error);
}

} // namespace sparenode::filesystem::detail

#endif
