#pragma once

#include <string>
#include <utility>

#include "sparenode/configuration/runtime/share_permissions.hpp"
#include "sparenode/configuration/shared_root.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Owns one immutable validated directory exposed by SpareNode.
class ShareConfig final
{
  public:
    /// @brief Creates a complete runtime share from validated settings.
    /// @param[in] name Human-readable share name decoded from configuration.
    /// @param[in] root Canonical filesystem boundary for every share operation.
    /// @param[in] permissions Explicit operations allowed inside the root.
    ShareConfig(std::string name, SharedRoot root, SharePermissions permissions)
        : name_(std::move(name)), root_(std::move(root)), permissions_(permissions)
    {
    }

    /// @brief Returns the share name.
    /// @return Immutable human-readable name.
    [[nodiscard]] const std::string &name() const noexcept
    {
        return name_;
    }

    /// @brief Returns the canonical filesystem boundary.
    /// @return Immutable validated shared root.
    [[nodiscard]] const SharedRoot &root() const noexcept
    {
        return root_;
    }

    /// @brief Returns the allowed filesystem operations.
    /// @return Immutable permission set.
    [[nodiscard]] const SharePermissions &permissions() const noexcept
    {
        return permissions_;
    }

  private:
    std::string name_;             ///< Human-readable validated share name.
    SharedRoot root_;              ///< Canonical filesystem boundary.
    SharePermissions permissions_; ///< Allowed operations inside the root.
};

} // namespace sparenode::configuration::runtime
