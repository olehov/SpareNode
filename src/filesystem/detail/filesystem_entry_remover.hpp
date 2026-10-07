#pragma once

#include <filesystem>

#include "sparenode/filesystem/detail/directory_entry_reader.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Deletes one child entry relative to a stable confined parent.
/// @param[in] parent Stable parent handle or descriptor inside the configured root.
/// @param[in] native_name Single native basename to delete without following a final link.
/// @return Success or a structured native deletion failure.
[[nodiscard]] Result<void, FilesystemEntryRemovalError>
remove_confined_entry(const ConfinedDirectory &parent, const std::filesystem::path &native_name);

} // namespace sparenode::filesystem::detail
