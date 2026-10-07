#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies why one requested directory could not be created.
enum class DirectoryCreationErrorCode : std::uint8_t
{
    invalid_destination, ///< The destination is the root or fails path validation.
    parent_unavailable,  ///< The destination parent cannot be opened inside the root.
    destination_exists,  ///< Another entry already owns the requested name.
    permission_denied,   ///< Host filesystem policy denies directory creation.
    creation_failed,     ///< The confined native creation operation failed.
    resource_failure     ///< Path handling exhausted memory.
};

/// @brief Preserves a portable creation category and its originating detail.
struct DirectoryCreationError
{
    DirectoryCreationErrorCode code{
        DirectoryCreationErrorCode::invalid_destination}; ///< Stable failure category.
    std::optional<SafePathError> path_error{};            ///< Invalid request-path detail.
    std::error_code system_error{};                       ///< Native failure, when available.
};

/// @brief Creates one directory through a stable parent confined to the shared root.
///
/// The untrusted destination passes through `SafePath`. Missing parent directories are not
/// created, and an existing file, directory, or link is never replaced.
/// @param[in] shared_root Validated root that must contain the destination parent.
/// @param[in] requested_path URL-encoded relative destination path.
/// @return Created path or a structured failure without exposing native paths.
[[nodiscard]] Result<std::filesystem::path, DirectoryCreationError>
create_directory(const configuration::SharedRoot &shared_root, std::string_view requested_path);

/// @brief Returns stable text for one directory-creation failure category.
/// @param[in] code Portable failure category.
/// @return Static English diagnostic text.
[[nodiscard]] const char *to_string(DirectoryCreationErrorCode code) noexcept;

} // namespace sparenode::filesystem
