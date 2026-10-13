#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/adapters/network_logging.hpp"
#include "sparenode/application/command_line.hpp"
#include "sparenode/application/running_application.hpp"
#include "sparenode/authentication/file_authentication.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/configuration/runtime/location_config.hpp"
#include "sparenode/http/api/filesystem_api.hpp"
#include "sparenode/http/routing/http_router.hpp"
#include "sparenode/http/session/http_connection_handler.hpp"
#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/logger.hpp"
#include "sparenode/version.hpp"

namespace
{

/// @brief Converts process arguments into the application's startup parser input.
/// @param[in] argc Number of arguments including the executable name.
/// @param[in] argv Non-null process argument pointers supplied by the runtime.
/// @return Parsed startup options or a structured command-line error.
[[nodiscard]] sparenode::Result<sparenode::application::StartupOptions,
                                sparenode::application::CommandLineError>
parse_startup_options(const int argc, const char *const *argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    return sparenode::application::parse_command_line(arguments);
}

/// @brief Creates mandatory owner authentication and reports only non-sensitive failures.
/// @param[in] credential_path Resolved owner credential file selected by configuration.
/// @param[in] locations Filesystem locations that must not expose the credential file.
/// @return Ready authentication service, or no value after reporting a startup failure.
[[nodiscard]] std::optional<sparenode::authentication::AuthenticationService>
create_authentication_or_report(
    const std::filesystem::path &credential_path,
    const std::span<const sparenode::configuration::runtime::LocationConfig> locations)
{
    auto result =
        sparenode::authentication::create_file_authentication_service(credential_path, locations);
    if (result)
    {
        return std::move(result).value();
    }

    const std::string diagnostic =
        std::string("error: ") + sparenode::authentication::to_string(result.error());
    sparenode::logging::write_console_diagnostic(std::cerr, diagnostic,
                                                 sparenode::logging::LogSeverity::error);
    std::cerr << "\nSpareNode was not started.\n";
    return std::nullopt;
}

} // namespace

int main(const int argc, const char *const argv[])
{
    const auto options = parse_startup_options(argc, argv);
    if (!options)
    {
        const std::string diagnostic =
            std::string("error: ") + sparenode::application::to_string(options.error().code);
        sparenode::logging::write_console_diagnostic(std::cerr, diagnostic,
                                                     sparenode::logging::LogSeverity::error);
        std::cerr << "\nusage: sparenode --config <path-to-spnode.conf>\n";
        return 2;
    }
    auto config_result = sparenode::configuration::ConfigLoader::load(options->config_path);
    if (!config_result)
    {
        sparenode::logging::write_console_diagnostic(
            std::cerr, sparenode::configuration::format_config_load_error(config_result.error()),
            sparenode::logging::LogSeverity::error);
        std::cerr << "\nSpareNode was not started.\n";
        return 2;
    }
    const auto authentication =
        create_authentication_or_report(config_result->servers().front().credentials_file(),
                                        config_result->servers().front().locations());
    if (!authentication.has_value())
    {
        return 3;
    }
    const auto console_sink = std::make_shared<sparenode::logging::ConsoleLogSink>(std::clog);
    const auto minimum_severity = config_result->servers().front().minimum_log_severity();
    const sparenode::logging::Logger logger(console_sink, minimum_severity);

    auto router_result =
        sparenode::http::make_filesystem_api_router(config_result->servers().front());
    if (!router_result)
    {
        const std::string diagnostic =
            std::string("error: ") + sparenode::http::to_string(router_result.error().code);
        sparenode::logging::write_console_diagnostic(std::cerr, diagnostic,
                                                     sparenode::logging::LogSeverity::error);
        std::cerr << "\nSpareNode was not started.\n";
        return 3;
    }
    const auto router =
        std::make_shared<const sparenode::http::HttpRouter>(std::move(router_result).value());
    const sparenode::http::HttpConnectionHandlerConfig handler_config{
        .timeouts = config_result->servers().front().http_timeouts(),
        .deadline_provider = {},
        .transport_policy = config_result->servers().front().transport_policy()};
    auto handler = sparenode::http::make_http_connection_handler(router, handler_config);
    auto admission = sparenode::http::make_http_connection_admission(handler_config);
    sparenode::application::RunningApplicationObservers observers{
        sparenode::logging::make_connection_failure_log_observer(logger),
        sparenode::logging::make_connection_server_failure_log_observer(logger)};
    auto application_result = sparenode::application::RunningApplication::start(
        std::move(config_result).value(), std::move(handler), std::move(observers),
        std::move(admission));
    if (!application_result)
    {
        const std::string diagnostic =
            "error: " +
            sparenode::application::format_application_start_error(application_result.error());
        sparenode::logging::write_console_diagnostic(std::cerr, diagnostic,
                                                     sparenode::logging::LogSeverity::error);
        std::cerr << "\nSpareNode was not started.\n";
        return 3;
    }

    logger.log(
        {sparenode::logging::LogSeverity::info, "application", "SpareNode startup complete"});
    std::cout << "SpareNode " << sparenode::version << " is running. Press Enter to stop.\n";
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    logger.log(
        {sparenode::logging::LogSeverity::info, "application", "SpareNode shutdown complete"});
    return 0;
}
