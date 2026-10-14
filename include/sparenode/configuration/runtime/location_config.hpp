#pragma once

#include <string>
#include <utility>

#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/filesystem/shared_root.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Owns one immutable validated directory exposed by SpareNode.
class LocationConfig final
{
  public:
    /// @brief Creates one filesystem-backed runtime location.
    /// @param[in] api_path Validated HTTP path prefix decoded from configuration.
    /// @param[in] root Canonical filesystem boundary for every location operation.
    /// @param[in] permissions Explicit operations allowed inside the root.
    LocationConfig(std::string api_path, filesystem::SharedRoot root,
                   filesystem::LocationPermissions permissions)
        : api_path_(std::move(api_path)), root_(std::move(root)), permissions_(permissions)
    {
    }

    /// @brief Returns the public HTTP path prefix.
    /// @return Immutable validated origin path.
    [[nodiscard]] const std::string &api_path() const noexcept
    {
        return api_path_;
    }

    /// @brief Returns the canonical filesystem boundary.
    /// @return Immutable validated shared root.
    [[nodiscard]] const filesystem::SharedRoot &root() const noexcept
    {
        return root_;
    }

    /// @brief Returns the allowed filesystem operations.
    /// @return Immutable permission set.
    [[nodiscard]] const filesystem::LocationPermissions &permissions() const noexcept
    {
        return permissions_;
    }

  private:
    std::string api_path_;                        ///< Validated public HTTP path prefix.
    filesystem::SharedRoot root_;                 ///< Canonical filesystem boundary.
    filesystem::LocationPermissions permissions_; ///< Allowed operations inside the root.
};

} // namespace sparenode::configuration::runtime
