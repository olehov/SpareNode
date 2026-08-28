#pragma once

#include <vector>

#include "sparenode/configuration/runtime/server_config.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Owns parser-independent settings used by a running SpareNode process.
struct AppConfig
{
    /// @brief Validated servers in configuration order.
    ///
    /// Version one maps exactly one server, while the collection keeps the runtime
    /// model ready for a future grammar that permits multiple server blocks.
    std::vector<ServerConfig> servers;
};

} // namespace sparenode::configuration::runtime
