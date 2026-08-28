#pragma once

namespace sparenode::configuration::runtime
{

/// @brief Stores the independent filesystem operations allowed for one share.
struct SharePermissions
{
    bool allow_read{true};    ///< Permits reading files and directory metadata.
    bool allow_write{false};  ///< Permits creating or replacing file content.
    bool allow_delete{false}; ///< Permits deleting files or directories.

    /// @brief Compares every permission flag.
    /// @param[in] other Permission set to compare with this instance.
    /// @return `true` when all three operation permissions match.
    [[nodiscard]] bool operator==(const SharePermissions &other) const noexcept = default;
};

} // namespace sparenode::configuration::runtime
