#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/configuration/environment_file.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why parsed variables could not form an application configuration.
enum class ApplicationConfigErrorCode : std::uint8_t
{
    missing_shared_root, ///< `SPARENODE_SHARED_ROOT` was not supplied.
    missing_value,       ///< `SPARENODE_SHARED_ROOT` has an empty value.
    invalid_shared_root, ///< The supplied shared-root path failed validation.
    invalid_boolean,     ///< A boolean variable is neither `true` nor `false`.
    invalid_log_severity ///< The configured logging threshold is unsupported.
};

/// @brief Describes a semantic application-configuration failure without throwing.
struct ApplicationConfigError
{
    ApplicationConfigErrorCode code =
        ApplicationConfigErrorCode::missing_shared_root; ///< Portable error category.
    std::string variable;                                ///< Variable associated with the failure.
    std::optional<SharedRootError> shared_root_error;    ///< Detailed path validation failure.
};

/// @brief Stores validated settings required to start SpareNode.
class ApplicationConfig final
{
  public:
    /// @brief Creates validated application settings from parsed environment variables.
    /// @param[in] environment Parsed variables from any supported environment file.
    /// @return Complete application configuration, or a structured parsing error.
    [[nodiscard]] static Result<ApplicationConfig, ApplicationConfigError>
    create(const EnvironmentFile &environment);

    /// @brief Returns the validated shared-root configuration.
    /// @return A stable reference valid for the lifetime of this object.
    [[nodiscard]] const SharedRoot &shared_root() const noexcept;

    /// @brief Reports whether the application should use multiple connection workers.
    /// @return `true` for a fixed multi-worker pool; `false` for one worker.
    [[nodiscard]] bool multithreading_enabled() const noexcept;

    /// @brief Returns the minimum severity emitted by application logging.
    /// @return Configured threshold, defaulting to `info` when omitted.
    [[nodiscard]] logging::LogSeverity minimum_log_severity() const noexcept;

  private:
    /// @brief Environment-file key that configures the shared-root directory.
    static constexpr std::string_view shared_root_variable_name = "SPARENODE_SHARED_ROOT";

    /// @brief Environment-file key that enables the multi-worker connection pool.
    static constexpr std::string_view multithreading_variable_name = "SPARENODE_MULTITHREADING";

    /// @brief Environment-file key that controls the minimum emitted log severity.
    static constexpr std::string_view log_level_variable_name = "SPARENODE_LOG_LEVEL";

    /// @brief Creates an application configuration from validated settings.
    /// @param[in] shared_root Directory exposed by the server.
    /// @param[in] multithreading_enabled Whether more than one worker may be configured.
    /// @param[in] minimum_log_severity Lowest severity forwarded to the logging sink.
    ApplicationConfig(SharedRoot shared_root, bool multithreading_enabled,
                      logging::LogSeverity minimum_log_severity);

    SharedRoot shared_root_;                   ///< Sole directory exposed by SpareNode v0.1.
    bool multithreading_enabled_{false};       ///< Enables a multi-worker connection pool.
    logging::LogSeverity minimum_log_severity_{///< Minimum emitted diagnostic severity.
                                               logging::LogSeverity::info};
};

/// @brief Returns a concise description of an application-configuration failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(ApplicationConfigErrorCode code) noexcept;

} // namespace sparenode::configuration
