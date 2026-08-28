#include "sparenode/application/command_line.hpp"

#include <optional>

namespace sparenode::application
{

Result<StartupOptions, CommandLineError>
parse_command_line(const std::span<const std::string_view> arguments)
{
    std::optional<std::filesystem::path> config_path;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        if (arguments[index] != "--config")
        {
            return unexpected(CommandLineError{CommandLineErrorCode::unknown_argument,
                                               std::string(arguments[index])});
        }
        if (config_path.has_value())
        {
            return unexpected(CommandLineError{CommandLineErrorCode::duplicate_config,
                                               std::string(arguments[index])});
        }
        if (index + 1 >= arguments.size())
        {
            return unexpected(
                CommandLineError{CommandLineErrorCode::missing_config_path, "--config"});
        }
        config_path = std::filesystem::path(arguments[++index]);
    }
    if (!config_path.has_value())
    {
        return unexpected(CommandLineError{CommandLineErrorCode::missing_config, {}});
    }
    return StartupOptions{std::move(*config_path)};
}

const char *to_string(const CommandLineErrorCode code) noexcept
{
    switch (code)
    {
    case CommandLineErrorCode::missing_config:
        return "the required --config option is missing";
    case CommandLineErrorCode::missing_config_path:
        return "--config requires a file path";
    case CommandLineErrorCode::duplicate_config:
        return "--config may be supplied only once";
    case CommandLineErrorCode::unknown_argument:
        return "unknown command-line argument";
    }
    return "unknown command-line error";
}

} // namespace sparenode::application
