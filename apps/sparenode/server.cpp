#include "server.hpp"

#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#include "sparenode/adapters/network_logging.hpp"
#include "sparenode/application/running_application.hpp"
#include "sparenode/bootstrap/runtime_bootstrap.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/http/api/filesystem_api.hpp"
#include "sparenode/http/routing/http_router.hpp"
#include "sparenode/http/transport/http_connection_handler.hpp"
#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/logger.hpp"
#include "sparenode/version.hpp"

namespace sparenode::app
{
namespace
{

/// @brief Projects the runtime server into the narrow filesystem API composition contract.
[[nodiscard]] http::FilesystemApiSettings
make_filesystem_api_settings(const application::runtime::ServerConfig &server)
{
    http::FilesystemApiSettings settings;
    settings.locations.reserve(server.locations().size());
    for (const auto &location : server.locations())
    {
        settings.locations.push_back(
            {location.api_path(), location.root(), location.permissions()});
    }
    settings.mime_types = server.mime_types();
    return settings;
}

/// @brief Writes one startup failure without allowing diagnostic line injection.
void write_startup_failure(const std::string &diagnostic)
{
    logging::write_console_diagnostic(std::cerr, diagnostic, logging::LogSeverity::error);
    std::cerr << "\nSpareNode was not started.\n";
}

} // namespace

int run_server(const application::StartupOptions &options)
{
    // Load and validate the configuration selected on the command line.
    auto config_result = configuration::ConfigLoader::load(options.config_path.string());
    if (!config_result)
    {
        write_startup_failure(configuration::format_config_load_error(config_result.error()));
        return 2;
    }

    // Resolve configuration-dependent resources and build the runtime ownership graph.
    auto runtime_result = bootstrap::RuntimeBootstrap::build(config_result.value());
    if (!runtime_result)
    {
        write_startup_failure(bootstrap::format_runtime_bootstrap_error(runtime_result.error()));
        return 3;
    }
    const auto &server = runtime_result->config().servers().front();
    const auto console_sink = std::make_shared<logging::ConsoleLogSink>(std::clog);
    const logging::Logger logger(console_sink, server.minimum_log_severity());

    // Compose the filesystem routes and the HTTP connection-processing pipeline.
    auto router_result = http::make_filesystem_api_router(make_filesystem_api_settings(server));
    if (!router_result)
    {
        write_startup_failure(std::string("error: ") + http::to_string(router_result.error().code));
        return 3;
    }
    const auto router = std::make_shared<const http::HttpRouter>(std::move(router_result).value());
    auto http_application = http::make_http_application(router);
    const http::HttpConnectionHandlerConfig handler_config{.timeouts = server.http_timeouts(),
                                                           .deadline_provider = {},
                                                           .transport_policy =
                                                               server.transport_policy()};
    auto handler = http::make_http_connection_handler(std::move(http_application), handler_config);
    auto admission = http::make_http_connection_admission(handler_config);
    application::RunningApplicationObservers observers{
        logging::make_connection_failure_log_observer(logger),
        logging::make_connection_server_failure_log_observer(logger)};

    // Transfer runtime resources into the running application and start its listeners.
    auto application_result = application::RunningApplication::start(
        std::move(runtime_result).value().release_config(), std::move(handler),
        std::move(observers), std::move(admission));
    if (!application_result)
    {
        write_startup_failure(
            "error: " + application::format_application_start_error(application_result.error()));
        return 3;
    }

    // Keep the process alive until the operator requests a graceful shutdown.
    logger.log({logging::LogSeverity::info, "application", "SpareNode startup complete"});
    std::cout << "SpareNode " << version << " is running. Press Enter to stop.\n";
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    logger.log({logging::LogSeverity::info, "application", "SpareNode shutdown complete"});
    return 0;
}

} // namespace sparenode::app
