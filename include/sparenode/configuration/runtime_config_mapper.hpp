#pragma once

#include <filesystem>

#include "sparenode/configuration/config_validator.hpp"
#include "sparenode/configuration/runtime/app_config.hpp"
#include "sparenode/http/mime_type_registry.hpp"

namespace sparenode::configuration
{

/// @brief Converts semantically validated parser output into runtime-only settings.
class RuntimeConfigMapper final
{
  public:
    /// @brief Applies configuration defaults and removes all parser metadata.
    /// @param[in] configuration Validated syntax and canonical location roots to map.
    /// @param[in] mime_types Validated mappings loaded by the startup pipeline.
    /// @param[in] credentials_file Resolved path to the separate owner credential file.
    /// @return Complete settings ready for application and server composition.
    [[nodiscard]] static runtime::AppConfig map(const ValidatedConfiguration &configuration,
                                                http::MimeTypeRegistry mime_types = {},
                                                std::filesystem::path credentials_file = {});
};

} // namespace sparenode::configuration
