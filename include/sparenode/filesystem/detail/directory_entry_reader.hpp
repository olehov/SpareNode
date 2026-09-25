#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Identifies an entry using metadata read from a stable confined handle.
enum class ConfinedEntryKind : std::uint8_t
{
    regular_file,  ///< A regular file contained by the configured root.
    directory,     ///< A directory contained by the configured root.
    symbolic_link, ///< A link whose opened target remains inside the configured root.
    other          ///< Another filesystem object that can be inspected safely.
};

/// @brief Owns metadata obtained from one stable confined filesystem handle.
struct ConfinedEntryMetadata
{
    ConfinedEntryKind kind{};                     ///< Portable entry category.
    std::optional<std::uintmax_t> size;           ///< Regular-file bytes, otherwise empty.
    std::chrono::sys_seconds modification_time{}; ///< Last modification time in UTC seconds.
};

/// @brief Groups paths required to open one directory entry inside its configured boundary.
struct ConfinedDirectoryEntryRequest
{
    const std::filesystem::path &shared_root; ///< Canonical filesystem boundary.
    const std::filesystem::path &directory;   ///< Validated directory containing the entry.
    const std::filesystem::path &native_name; ///< Single native filename relative to directory.
};

/// @brief Opens one directory entry and verifies its stable target against a shared root.
/// @param[in] request Boundary, validated parent directory, and native filename.
/// @return Metadata, an empty optional for a changed or unsafe entry, or a directory-level error.
[[nodiscard]] Result<std::optional<ConfinedEntryMetadata>, std::error_code>
read_confined_directory_entry(const ConfinedDirectoryEntryRequest &request);

} // namespace sparenode::filesystem::detail
