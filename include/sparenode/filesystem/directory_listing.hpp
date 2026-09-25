#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies the portable kind of one safely listed directory entry.
enum class DirectoryEntryType : std::uint8_t
{
    regular_file,  ///< Regular file whose payload size is available.
    directory,     ///< Directory contained by the configured shared root.
    symbolic_link, ///< Symbolic link whose resolved target remains inside the shared root.
    other          ///< Supported filesystem object outside the common file/directory kinds.
};

/// @brief Owns metadata exposed for one sandbox-confined directory child.
struct DirectoryListingEntry
{
    std::string name;                             ///< UTF-8 basename without a host path.
    DirectoryEntryType type{};                    ///< Portable entry category.
    std::optional<std::uintmax_t> size;           ///< Regular-file bytes, otherwise empty.
    std::chrono::sys_seconds modification_time{}; ///< Last modification time in UTC seconds.
};

/// @brief Identifies why a directory could not be listed safely.
enum class DirectoryListingErrorCode : std::uint8_t
{
    invalid_path,               ///< The untrusted request failed SafePath validation.
    not_found,                  ///< The requested directory does not exist.
    not_directory,              ///< The requested path does not identify a directory.
    permission_denied,          ///< The host denied access to required metadata.
    too_many_entries,           ///< The directory exceeds the bounded listing policy.
    filesystem_failure,         ///< A host filesystem operation failed unexpectedly.
    resource_allocation_failed, ///< Listing-owned memory could not be allocated.
};

/// @brief Preserves portable and subsystem-specific directory listing failure details.
struct DirectoryListingError
{
    DirectoryListingErrorCode code{};            ///< Stable listing failure category.
    std::error_code system_error{};              ///< Optional native filesystem detail.
    std::optional<SafePathErrorCode> path_error; ///< Optional SafePath rejection category.
};

/// @brief Maximum number of host entries inspected for one request.
inline constexpr std::size_t maximum_directory_listing_entries = 512;

/// @brief Lists one untrusted relative directory without escaping its shared root.
/// @param[in] shared_root Validated root containing every returned entry.
/// @param[in] requested_path URL-encoded relative directory path, or empty for the root.
/// @return Name-sorted metadata or a structured safe failure.
[[nodiscard]] Result<std::vector<DirectoryListingEntry>, DirectoryListingError>
list_directory(const configuration::SharedRoot &shared_root, std::string_view requested_path);

/// @brief Returns stable text for one directory listing failure category.
/// @param[in] code Failure category to describe.
/// @return Static diagnostic text.
[[nodiscard]] const char *to_string(DirectoryListingErrorCode code) noexcept;

} // namespace sparenode::filesystem
