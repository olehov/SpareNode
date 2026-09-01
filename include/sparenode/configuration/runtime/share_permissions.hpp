#pragma once

namespace sparenode::configuration::runtime
{

/// @brief Stores immutable filesystem operations allowed for one share.
class SharePermissions final
{
  public:
    /// @brief Creates one complete permission set.
    /// @param[in] allow_read Permits reading files and directory metadata.
    /// @param[in] allow_write Permits creating or replacing file content.
    /// @param[in] allow_delete Permits deleting files or directories.
    explicit constexpr SharePermissions(const bool allow_read = true,
                                        const bool allow_write = false,
                                        const bool allow_delete = false) noexcept
        : allow_read_(allow_read), allow_write_(allow_write), allow_delete_(allow_delete)
    {
    }

    /// @brief Reports whether read operations are permitted.
    /// @return `true` when files and directory metadata may be read.
    [[nodiscard]] constexpr bool allows_read() const noexcept
    {
        return allow_read_;
    }

    /// @brief Reports whether write operations are permitted.
    /// @return `true` when file content may be created or replaced.
    [[nodiscard]] constexpr bool allows_write() const noexcept
    {
        return allow_write_;
    }

    /// @brief Reports whether delete operations are permitted.
    /// @return `true` when files or directories may be deleted.
    [[nodiscard]] constexpr bool allows_delete() const noexcept
    {
        return allow_delete_;
    }

    /// @brief Compares every permission flag.
    /// @param[in] other Permission set to compare with this instance.
    /// @return `true` when all three operation permissions match.
    [[nodiscard]] bool operator==(const SharePermissions &other) const noexcept = default;

  private:
    bool allow_read_;   ///< Immutable read permission.
    bool allow_write_;  ///< Immutable write permission.
    bool allow_delete_; ///< Immutable delete permission.
};

} // namespace sparenode::configuration::runtime
