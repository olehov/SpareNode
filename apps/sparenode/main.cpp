#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/adapters/network_logging.hpp"
#include "sparenode/application/command_line.hpp"
#include "sparenode/application/running_application.hpp"
#include "sparenode/bootstrap/runtime_bootstrap.hpp"
#include "sparenode/configuration/config_loader.hpp"
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
    auto config_result =
        sparenode::configuration::ConfigLoader::load(options->config_path.string());
    if (!config_result)
    {
        sparenode::logging::write_console_diagnostic(
            std::cerr, sparenode::configuration::format_config_load_error(config_result.error()),
            sparenode::logging::LogSeverity::error);
        std::cerr << "\nSpareNode was not started.\n";
        return 2;
    }
    auto runtime_result = sparenode::bootstrap::RuntimeBootstrap::build(config_result.value());
    if (!runtime_result)
    {
        sparenode::logging::write_console_diagnostic(
            std::cerr, sparenode::bootstrap::format_runtime_bootstrap_error(runtime_result.error()),
            sparenode::logging::LogSeverity::error);
        std::cerr << "\nSpareNode was not started.\n";
        return 3;
    }
    const auto console_sink = std::make_shared<sparenode::logging::ConsoleLogSink>(std::clog);
    const auto minimum_severity = runtime_result->config().servers().front().minimum_log_severity();
    const sparenode::logging::Logger logger(console_sink, minimum_severity);

    auto router_result =
        sparenode::http::make_filesystem_api_router(runtime_result->config().servers().front());
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
        .timeouts = runtime_result->config().servers().front().http_timeouts(),
        .deadline_provider = {},
        .transport_policy = runtime_result->config().servers().front().transport_policy()};
    auto handler = sparenode::http::make_http_connection_handler(router, handler_config);
    auto admission = sparenode::http::make_http_connection_admission(handler_config);
    sparenode::application::RunningApplicationObservers observers{
        sparenode::logging::make_connection_failure_log_observer(logger),
        sparenode::logging::make_connection_server_failure_log_observer(logger)};
    auto application_result = sparenode::application::RunningApplication::start(
        std::move(runtime_result).value().release_config(), std::move(handler),
        std::move(observers), std::move(admission));
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
