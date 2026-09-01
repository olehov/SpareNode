#pragma once

#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/server_config.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Owns immutable parser-independent settings used by a SpareNode process.
class AppConfig final
{
  public:
    /// @brief Creates an empty configuration for explicit startup validation.
    AppConfig() = default;

    /// @brief Creates complete runtime settings from validated servers.
    /// @param[in] servers Validated servers in configuration order.
    explicit AppConfig(std::vector<ServerConfig> servers) : servers_(std::move(servers))
    {
    }

    /// @brief Validated servers in configuration order.
    ///
    /// Version one maps exactly one server, while the collection keeps the runtime
    /// model ready for a future grammar that permits multiple server blocks.
    /// @return Immutable server settings.
    [[nodiscard]] const std::vector<ServerConfig> &servers() const noexcept
    {
        return servers_;
    }

  private:
    std::vector<ServerConfig> servers_; ///< Validated servers in configuration order.
};

} // namespace sparenode::configuration::runtime
