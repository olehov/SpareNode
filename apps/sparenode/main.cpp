#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "sparenode/configuration/application_config.hpp"
#include "sparenode/configuration/environment_file.hpp"
#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/logger.hpp"
#include "sparenode/version.hpp"

namespace
{

/// @brief Logs the versioned application startup event without propagating diagnostics failures.
/// @param[in] logger Configured application logger.
void log_application_startup(const sparenode::logging::Logger &logger) noexcept
{
    try
    {
        std::string message = "SpareNode version=";
        message.append(sparenode::version);
        message.append(" startup complete");
        logger.log({sparenode::logging::LogSeverity::info, "application", message});
    }
    catch (...)
    {
        // Startup remains valid even when diagnostics cannot be constructed.
        return;
    }
}

/// @brief Logs a structured environment-file failure without exposing variable values.
/// @param[in] logger Diagnostic destination used during bootstrap.
/// @param[in] error File error returned by the universal environment loader.
void log_environment_file_error(
    const sparenode::logging::Logger &logger,
    const sparenode::configuration::EnvironmentFileError &error) noexcept
{
    try
    {
        std::ostringstream message;
        message << sparenode::configuration::to_string(error.code)
                << " source=" << error.source_path.string();
        if (error.line_number != 0)
        {
            message << " line=" << error.line_number;
        }
        if (!error.variable.empty())
        {
            message << " variable=" << error.variable;
        }

        logger.log({sparenode::logging::LogSeverity::error, "configuration", message.str()});
    }
    catch (...)
    {
        // Diagnostic construction must not replace the original startup result.
        return;
    }
}

/// @brief Logs a semantic configuration failure without exposing configured values.
/// @param[in] logger Diagnostic destination used during bootstrap.
/// @param[in] error Error returned while interpreting parsed environment variables.
void log_application_config_error(
    const sparenode::logging::Logger &logger,
    const sparenode::configuration::ApplicationConfigError &error) noexcept
{
    try
    {
        std::ostringstream message;
        message << sparenode::configuration::to_string(error.code);
        if (!error.variable.empty())
        {
            message << " variable=" << error.variable;
        }
        if (error.shared_root_error.has_value())
        {
            message << " shared_root_error="
                    << sparenode::configuration::to_string(error.shared_root_error->code);
        }

        logger.log({sparenode::logging::LogSeverity::error, "configuration", message.str()});
    }
    catch (...)
    {
        // Diagnostic construction must not replace the original startup result.
        return;
    }
}

/// @brief Logs validated startup settings without exposing the shared-root path.
/// @param[in] logger Configured application logger.
/// @param[in] configuration Validated settings to summarize.
void log_startup_configuration(
    const sparenode::logging::Logger &logger,
    const sparenode::configuration::ApplicationConfig &configuration) noexcept
{
    try
    {
        std::ostringstream message;
        message << "configuration loaded shared_root=validated multithreading="
                << (configuration.multithreading_enabled() ? "enabled" : "disabled")
                << " minimum_log_severity="
                << sparenode::logging::to_string(configuration.minimum_log_severity());
        logger.log({sparenode::logging::LogSeverity::info, "configuration", message.str()});
    }
    catch (...)
    {
        // Startup remains valid even when diagnostics cannot be constructed.
        return;
    }
}

} // namespace

/// @brief Loads `.env`, validates startup configuration, and logs the application lifecycle.
/// @return Zero on success, or two when the startup configuration is invalid.
int main()
{
    const auto console_sink = std::make_shared<sparenode::logging::ConsoleLogSink>(std::clog);
    const sparenode::logging::Logger bootstrap_logger(console_sink);

    const auto environment_result = sparenode::configuration::EnvironmentFile::load_default();
    if (!environment_result)
    {
        log_environment_file_error(bootstrap_logger, environment_result.error());
        return 2;
    }

    const auto configuration_result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());
    if (!configuration_result)
    {
        log_application_config_error(bootstrap_logger, configuration_result.error());
        return 2;
    }

    const sparenode::logging::Logger logger(console_sink,
                                            configuration_result->minimum_log_severity());
    log_application_startup(logger);
    log_startup_configuration(logger, configuration_result.value());
    logger.log(
        {sparenode::logging::LogSeverity::info, "application", "SpareNode shutdown complete"});
    return 0;
}
