#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/filesystem/temporary_file.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies why a completed temporary upload could not be published.
enum class UploadFinalizationErrorCode : std::uint8_t
{
    invalid_source,       ///< The source has not completed or has no owned path.
    invalid_destination,  ///< The destination is the root or fails path validation.
    parent_unavailable,   ///< The destination parent cannot be opened inside the root.
    destination_exists,   ///< Another entry already owns the requested name.
    source_read_failed,   ///< The completed source could not be read consistently.
    staging_failed,       ///< A private sibling staging file could not be created.
    staging_write_failed, ///< Copying into sibling staging storage failed.
    staging_flush_failed, ///< The sibling staging file could not be flushed.
    publish_failed,       ///< Atomic, no-replace publication failed.
    cancelled,            ///< The caller cancelled before publication.
    deadline_exceeded,    ///< The caller's deadline expired before publication.
    resource_failure      ///< Buffer or path allocation failed.
};

/// @brief Preserves a portable finalization category and its originating detail.
struct UploadFinalizationError
{
    UploadFinalizationErrorCode code{
        UploadFinalizationErrorCode::invalid_source}; ///< Stable failure category.
    std::optional<SafePathError> path_error{};        ///< Populated for invalid request paths.
    std::error_code system_error{};                   ///< Native failure, when available.
};

/// @brief Copies a completed request artifact to a private sibling and publishes it atomically.
///
/// The destination is validated from the untrusted relative path. Publication never replaces
/// an existing entry; all failed attempts remove their private sibling on a best-effort basis.
/// The source remains owned by its `TemporaryFile` and is removed when that owner is destroyed.
/// @param[in] shared_root Validated root that must contain the destination parent.
/// @param[in] requested_path URL-encoded relative destination path.
/// @param[in] source Completed request-body artifact retained by the caller.
/// @param[in] options Cancellation and deadline observed between bounded native operations.
/// @return Published path or a structured failure; no partial destination is ever published.
[[nodiscard]] Result<std::filesystem::path, UploadFinalizationError>
finalize_upload(const configuration::SharedRoot &shared_root, std::string_view requested_path,
                const TemporaryFile &source, const TemporaryFileIoOptions &options = {});

/// @brief Returns stable text for an upload finalization failure category.
/// @param[in] code Portable failure category.
/// @return Static English diagnostic text.
[[nodiscard]] const char *to_string(UploadFinalizationErrorCode code) noexcept;

} // namespace sparenode::filesystem
