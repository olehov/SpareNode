#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/configuration/environment_file.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why parsed variables could not form an application configuration.
enum class ApplicationConfigErrorCode : std::uint8_t
{
    missing_shared_root, ///< `SPARENODE_SHARED_ROOT` was not supplied.
    missing_value,       ///< `SPARENODE_SHARED_ROOT` has an empty value.
    invalid_shared_root  ///< The supplied shared-root path failed validation.
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

  private:
    /// @brief Environment-file key that configures the shared-root directory.
    static constexpr std::string_view shared_root_variable_name = "SPARENODE_SHARED_ROOT";

    /// @brief Creates an application configuration with one validated shared root.
    /// @param[in] shared_root Directory exposed by the server.
    explicit ApplicationConfig(SharedRoot shared_root);

    SharedRoot shared_root_; ///< Sole directory exposed by SpareNode v0.1.
};

/// @brief Returns a concise description of an application-configuration failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(ApplicationConfigErrorCode code) noexcept;

} // namespace sparenode::configuration
