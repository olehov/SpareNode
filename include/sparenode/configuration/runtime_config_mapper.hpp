#pragma once

#include "sparenode/configuration/config_validator.hpp"
#include "sparenode/configuration/runtime/app_config.hpp"

namespace sparenode::configuration
{

/// @brief Converts semantically validated parser output into runtime-only settings.
class RuntimeConfigMapper final
{
  public:
    /// @brief Applies configuration defaults and removes all parser metadata.
    /// @param[in] configuration Validated syntax and canonical share roots to map.
    /// @return Complete settings ready for application and server composition.
    [[nodiscard]] static runtime::AppConfig map(const ValidatedConfiguration &configuration);
};

} // namespace sparenode::configuration
