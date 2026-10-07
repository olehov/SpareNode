#include "filesystem_entry_remover.hpp"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

#include "posix_file_descriptor.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Converts the current errno into the portable deletion contract.
[[nodiscard]] FilesystemEntryRemovalError failure() noexcept
{
    auto code = FilesystemEntryRemovalErrorCode::removal_failed;
    if (errno == ENOENT || errno == ENOTDIR)
    {
        code = FilesystemEntryRemovalErrorCode::not_found;
    }
    else if (errno == ENOTEMPTY || errno == EEXIST || errno == EBUSY)
    {
        code = FilesystemEntryRemovalErrorCode::directory_not_empty;
    }
    else if (errno == EACCES || errno == EPERM || errno == EROFS)
    {
        code = FilesystemEntryRemovalErrorCode::permission_denied;
    }
    return {code, {}, last_posix_error()};
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
    if (::unlinkat(parent.native_handle(), native_name.c_str(), 0) == 0)
    {
        return {};
    }
    if (errno == EISDIR &&
        ::unlinkat(parent.native_handle(), native_name.c_str(), AT_REMOVEDIR) == 0)
    {
        return {};
    }
    return unexpected(failure());
}

} // namespace sparenode::filesystem::detail

#endif
