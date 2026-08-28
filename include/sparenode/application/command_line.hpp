#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "sparenode/result.hpp"

namespace sparenode::application
{

/// @brief Identifies why startup arguments could not select one configuration file.
enum class CommandLineErrorCode : std::uint8_t
{
    missing_config,      ///< The required `--config` option was omitted.
    missing_config_path, ///< `--config` has no following path.
    duplicate_config,    ///< `--config` was supplied more than once.
    unknown_argument,    ///< An unsupported startup argument was supplied.
};

/// @brief Describes one invalid startup argument without interpreting configuration.
struct CommandLineError
{
    CommandLineErrorCode code{}; ///< Portable command-line failure category.
    std::string argument;        ///< Offending argument, or empty when one is missing.
};

/// @brief Stores the single explicit source of persistent startup configuration.
struct StartupOptions
{
    std::filesystem::path config_path; ///< File supplied through `--config`.
};

/// @brief Parses application arguments after the executable name.
/// @param[in] arguments Ordered command-line arguments excluding `argv[0]`.
/// @return Selected configuration path, or a structured usage failure.
[[nodiscard]] Result<StartupOptions, CommandLineError>
parse_command_line(std::span<const std::string_view> arguments);

/// @brief Returns a concise description of a command-line failure.
/// @param[in] code Portable argument failure category.
/// @return Static English text suitable for stderr diagnostics.
[[nodiscard]] const char *to_string(CommandLineErrorCode code) noexcept;

} // namespace sparenode::application
