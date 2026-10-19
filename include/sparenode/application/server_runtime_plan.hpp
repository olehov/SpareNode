#pragma once

#include <utility>

#include "sparenode/network/connection_server.hpp"

namespace sparenode::application
{

/// @brief Owns every dependency required to start one independent connection server.
class ServerRuntimePlan final
{
  public:
    /// @brief Stores a fully composed listener, dispatcher, policy, and observer configuration.
    /// @param[in] config Complete network server configuration transferred into the plan.
    explicit ServerRuntimePlan(network::ConnectionServerConfig config) noexcept
        : config_(std::move(config))
    {
    }

    /// @brief Transfers the complete configuration to the connection-server runtime.
    /// @return Independently composed configuration for exactly one server.
    [[nodiscard]] network::ConnectionServerConfig release_config() && noexcept
    {
        return std::move(config_);
    }

  private:
    network::ConnectionServerConfig config_; ///< Complete per-server runtime dependencies.
};

} // namespace sparenode::application
