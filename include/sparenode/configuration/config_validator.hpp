#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sparenode/configuration/directives/parsed_location_directive.hpp"
#include "sparenode/configuration/directives/parsed_server_directive.hpp"
#include "sparenode/configuration/parsed_config.hpp"
#include "sparenode/configuration/settings.hpp"
#include "sparenode/foundation/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies a semantic rule rejected after successful parsing.
enum class ConfigValidationErrorCode : std::uint8_t
{
    duplicate_server_directive,   ///< A singleton server directive appears more than once.
    missing_location,             ///< The server contains no filesystem location.
    invalid_bind_address,         ///< `bind` is not a numeric IPv4 or IPv6 address.
    port_out_of_range,            ///< `port` is outside 1 through 65535.
    missing_worker_threads,       ///< Multithreading is enabled without a worker count.
    unexpected_worker_threads,    ///< A worker count is present while multithreading is disabled.
    worker_threads_out_of_range,  ///< The enabled worker count is outside 2 through 64.
    invalid_log_level,            ///< `log_level` is not a supported severity.
    invalid_mime_types_file,      ///< `mime_types_file` is empty.
    invalid_credentials_file,     ///< `credentials_file` is empty.
    timeout_out_of_range,         ///< A receive timeout is outside 1 through 86400000 milliseconds.
    invalid_transport_mode,       ///< `transport_mode` is not a supported deployment mode.
    invalid_trusted_proxy,        ///< `trusted_proxy` is not one numeric IP address.
    missing_trusted_proxy,        ///< Proxy HTTPS mode has no trusted proxy address.
    unexpected_trusted_proxy,     ///< A trusted proxy is configured outside proxy mode.
    unsafe_transport_bind,        ///< Listener exposure conflicts with its transport mode.
    invalid_location_path,        ///< A location argument is not a supported HTTP origin path.
    conflicting_location_path,    ///< Two location paths overlap or are duplicates.
    duplicate_location_directive, ///< A singleton location directive appears more than once.
    missing_location_root,        ///< A location contains no required filesystem `path`.
    invalid_location_root,        ///< A filesystem `path` is empty.
};

/// @brief Describes one deterministic semantic configuration failure.
struct ConfigValidationError
{
    /// @brief Creates a semantic error at its primary source location.
    /// @param[in] error_code Stable semantic failure category.
    /// @param[in] source_location Token or delimiter associated with the failure.
    ConfigValidationError(ConfigValidationErrorCode error_code,
                          const SourceLocation &source_location);

    ConfigValidationErrorCode code{}; ///< Stable failure category.
    SourceLocation location;          ///< Token or closing brace associated with the failure.
    std::optional<directives::ServerDirectiveKind> server_directive;     ///< Related server field.
    std::optional<directives::LocationDirectiveKind> location_directive; ///< Related field.
    std::optional<std::string> location_path; ///< Public path when a block is involved.
};

/// @brief Owns parsed syntax that has passed every version-one semantic rule.
///
/// Only ConfigValidator can create this type. A later mapping stage can therefore
/// require it instead of accepting unchecked parser output.
class ValidatedConfiguration final
{
  public:
    /// @brief Returns the validated parsed representation without transferring ownership.
    /// @return Stable reference valid for the lifetime of this object.
    [[nodiscard]] const ParsedConfiguration &parsed() const noexcept;

    /// @brief Transfers the validated parsed representation to the next configuration stage.
    /// @return Owned parser model carrying the validator's success guarantee.
    [[nodiscard]] ParsedConfiguration release_parsed() && noexcept;

    /// @brief Returns implementation-independent settings produced by semantic validation.
    /// @return Primitive settings safe for environment-dependent bootstrap construction.
    [[nodiscard]] const AppSettings &settings() const noexcept;

    /// @brief Transfers implementation-independent settings to bootstrap.
    /// @return Owned validated settings without parser implementation dependencies.
    [[nodiscard]] AppSettings release_settings() && noexcept;

  private:
    friend class ConfigValidator;

    /// @brief Stores parser output after all semantic checks have succeeded.
    /// @param[in] parsed_configuration Complete validated parser model.
    /// @param[in] settings Validated primitive settings corresponding to parser input.
    ValidatedConfiguration(ParsedConfiguration parsed_configuration, AppSettings settings);

    ParsedConfiguration parsed_; ///< Syntax tree protected by the validation boundary.
    AppSettings settings_;       ///< Validated primitive output consumed by bootstrap.
};

/// @brief Applies version-one semantic and filesystem rules to parsed configuration.
class ConfigValidator final
{
  public:
    /// @brief Validates complete parser output without starting server components.
    /// @param[in] configuration Parsed configuration transferred into validation.
    /// @return Validated ownership wrapper, or every independently detectable error.
    [[nodiscard]] static Result<ValidatedConfiguration, std::vector<ConfigValidationError>>
    validate(ParsedConfiguration configuration);
};

/// @brief Returns a concise description of a semantic validation failure.
/// @param[in] code Portable failure category to describe.
/// @return Static English text suitable for user-facing diagnostics.
[[nodiscard]] const char *to_string(ConfigValidationErrorCode code) noexcept;

} // namespace sparenode::configuration
