#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies why one requested filesystem entry could not be deleted.
enum class FilesystemEntryRemovalErrorCode : std::uint8_t
{
    invalid_destination, ///< The destination is the root or fails path validation.
    parent_unavailable,  ///< The destination parent cannot be opened inside the root.
    not_found,           ///< No deletable entry owns the requested name.
    directory_not_empty, ///< Recursive directory deletion is intentionally unsupported.
    permission_denied,   ///< Host filesystem policy denies deletion.
    removal_failed,      ///< The confined native deletion operation failed.
    resource_failure     ///< Path handling exhausted memory.
};

/// @brief Preserves a portable deletion category and its originating detail.
struct FilesystemEntryRemovalError
{
    FilesystemEntryRemovalErrorCode code{
        FilesystemEntryRemovalErrorCode::invalid_destination}; ///< Stable failure category.
    std::optional<SafePathError> path_error{};                 ///< Invalid request-path detail.
    std::error_code system_error{};                            ///< Native failure, when available.
};

/// @brief Deletes one file, link, or empty directory through a confined parent.
///
/// The shared root itself and non-empty directories are never deleted. A final symbolic link or
/// junction is removed as an entry without following it, after `SafePath` verifies that its target
/// remains inside the shared root.
/// @param[in] shared_root Validated root that must contain the requested entry and its target.
/// @param[in] requested_path URL-encoded relative path naming the entry to delete.
/// @return Success or a structured failure without exposing native paths.
[[nodiscard]] Result<void, FilesystemEntryRemovalError>
remove_filesystem_entry(const configuration::SharedRoot &shared_root,
                        std::string_view requested_path);

/// @brief Returns stable text for one filesystem-deletion failure category.
/// @param[in] code Portable failure category.
/// @return Static English diagnostic text.
[[nodiscard]] const char *to_string(FilesystemEntryRemovalErrorCode code) noexcept;

} // namespace sparenode::filesystem
