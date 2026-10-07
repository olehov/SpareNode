#pragma once

#include <filesystem>

#include "sparenode/filesystem/detail/directory_entry_reader.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Exclusively creates one child directory relative to a confined parent.
/// @param[in] parent Stable parent handle or descriptor inside the configured root.
/// @param[in] native_name Single native basename to create.
/// @return Success or a structured native creation failure.
[[nodiscard]] Result<void, DirectoryCreationError>
create_confined_directory(const ConfinedDirectory &parent,
                          const std::filesystem::path &native_name);

} // namespace sparenode::filesystem::detail
