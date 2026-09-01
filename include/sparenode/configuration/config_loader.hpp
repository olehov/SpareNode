#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/config_parser.hpp"
#include "sparenode/configuration/config_validator.hpp"
#include "sparenode/configuration/runtime/app_config.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies a filesystem operation that prevented configuration loading.
enum class ConfigFileErrorCode : std::uint8_t
{
    open_failed, ///< The selected configuration file could not be opened.
    read_failed, ///< The opened configuration file could not be read completely.
};

/// @brief Describes a configuration-file I/O failure without losing its source path.
struct ConfigFileError
{
    ConfigFileErrorCode code{}; ///< Portable file failure category.
};

/// @brief Preserves the exact stage-specific failure produced by configuration loading.
using ConfigLoadFailure = std::variant<ConfigFileError, ConfigLexerError, ConfigParserError,
                                       std::vector<ConfigValidationError>>;

/// @brief Associates a structured configuration failure with its selected source file.
struct ConfigLoadError
{
    std::filesystem::path source_path; ///< File selected by the application user.
    ConfigLoadFailure failure;         ///< Original file, lexer, parser, or validator failure.
};

/// @brief Loads the complete persistent configuration pipeline without starting resources.
class ConfigLoader final
{
  public:
    /// @brief Reads, tokenizes, parses, validates, and maps one `spnode.conf` file.
    /// @param[in] source_path Configuration file selected at application startup.
    /// @return Parser-independent runtime settings, or the original stage failure.
    [[nodiscard]] static Result<runtime::AppConfig, ConfigLoadError>
    load(const std::filesystem::path &source_path);
};

/// @brief Formats all diagnostics retained by one configuration-load failure.
/// @param[in] error Structured failure and its source-file path.
/// @return User-facing diagnostics without configuration values or file contents.
[[nodiscard]] std::string format_config_load_error(const ConfigLoadError &error);

/// @brief Returns a concise description of a configuration-file failure.
/// @param[in] code Portable file error category to describe.
/// @return Static English text suitable for startup diagnostics.
[[nodiscard]] const char *to_string(ConfigFileErrorCode code) noexcept;

} // namespace sparenode::configuration
