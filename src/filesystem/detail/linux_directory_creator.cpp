#include "directory_creator.hpp"

#ifndef _WIN32

#include <cerrno>
#include <sys/stat.h>
#include <system_error>

#include "posix_file_descriptor.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Converts the current errno into the portable creation contract.
[[nodiscard]] DirectoryCreationError failure() noexcept
{
    auto code = DirectoryCreationErrorCode::creation_failed;
    if (errno == EEXIST)
    {
        code = DirectoryCreationErrorCode::destination_exists;
    }
    else if (errno == ENOENT || errno == ENOTDIR)
    {
        code = DirectoryCreationErrorCode::parent_unavailable;
    }
    else if (errno == EACCES || errno == EPERM || errno == EROFS)
    {
        code = DirectoryCreationErrorCode::permission_denied;
    }
    return {code, {}, last_posix_error()};
}

} // namespace

/// @brief Creates one owner-private directory through a stable parent descriptor.
Result<void, DirectoryCreationError>
create_confined_directory(const ConfinedDirectory &parent, const std::filesystem::path &native_name)
{
    if (native_name.empty() || native_name != native_name.filename())
    {
        return unexpected(DirectoryCreationError{DirectoryCreationErrorCode::invalid_destination});
    }
    constexpr mode_t directory_mode = S_IRWXU;
    if (::mkdirat(parent.native_handle(), native_name.c_str(), directory_mode) != 0)
    {
        return unexpected(failure());
    }
    return {};
}

} // namespace sparenode::filesystem::detail

#endif
