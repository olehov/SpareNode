#pragma once

#include <string>

#include "sparenode/configuration/runtime/share_permissions.hpp"
#include "sparenode/configuration/shared_root.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Describes one validated directory exposed by SpareNode.
struct ShareConfig
{
    std::string name;             ///< Human-readable share name decoded from configuration.
    SharedRoot root;              ///< Canonical filesystem boundary for every share operation.
    SharePermissions permissions; ///< Explicit operations allowed inside the root.
};

} // namespace sparenode::configuration::runtime
