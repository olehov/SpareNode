#include "server.hpp"

#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

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

/// @brief Maximum connections waiting in the operating-system accept queue.
constexpr int listen_backlog = 128;

/// @brief Maximum accepted connections waiting for a request worker per server.
constexpr std::size_t pending_connection_limit = 128;

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

/// @brief Composes every transport and application dependency for one server.
[[nodiscard]] Result<application::ServerRuntimePlan, http::FilesystemApiError>
make_server_runtime_plan(const application::runtime::ServerConfig &server,
                         const std::shared_ptr<logging::LogSink> &console_sink)
{
    const logging::Logger logger(console_sink, server.minimum_log_severity());
    auto router_result = http::make_filesystem_api_router(make_filesystem_api_settings(server));
    if (!router_result)
    {
        return unexpected(router_result.error());
    }

    const auto router = std::make_shared<const http::HttpRouter>(std::move(router_result).value());
    auto http_application = http::make_http_application(router);
    const http::HttpConnectionHandlerConfig handler_config{.timeouts = server.http_timeouts(),
                                                           .deadline_provider = {},
                                                           .transport_policy =
                                                               server.transport_policy()};
    auto handler = http::make_http_connection_handler(std::move(http_application), handler_config);
    auto admission = http::make_http_connection_admission(handler_config);
    return application::ServerRuntimePlan(network::ConnectionServerConfig{
        server.endpoint(),
        listen_backlog,
        server.multithreading_enabled(),
        {{server.worker_threads(), pending_connection_limit},
         std::move(handler),
         logging::make_connection_failure_log_observer(logger),
         std::move(admission)},
        logging::make_connection_server_failure_log_observer(logger)});
}

/// @brief Composes independent runtime plans for every validated server.
[[nodiscard]] Result<std::vector<application::ServerRuntimePlan>, http::FilesystemApiError>
make_server_runtime_plans(const application::runtime::AppConfig &config,
                          const std::shared_ptr<logging::LogSink> &console_sink)
{
    std::vector<application::ServerRuntimePlan> plans;
    plans.reserve(config.servers().size());
    for (const auto &server : config.servers())
    {
        auto plan = make_server_runtime_plan(server, console_sink);
        if (!plan)
        {
            return unexpected(plan.error());
        }
        plans.push_back(std::move(plan).value());
    }
    return plans;
}

} // namespace

/// @brief Loads configuration, composes server plans, and owns the process lifecycle.
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
    const auto console_sink = std::make_shared<logging::ConsoleLogSink>(std::clog);
    const logging::Logger process_logger(console_sink, runtime_result->process_log_severity());

    // Compose routes, HTTP policies, admission, and observers independently per server.
    auto plans_result = make_server_runtime_plans(runtime_result->config(), console_sink);
    if (!plans_result)
    {
        write_startup_failure(std::string("error: ") + http::to_string(plans_result.error().code));
        return 3;
    }

    // Transfer runtime resources into the running application and start its listeners.
    // RuntimePlan remains alive here so its process-wide authentication context outlives servers.
    auto application_result =
        application::RunningApplication::start(std::move(plans_result).value());
    if (!application_result)
    {
        write_startup_failure(
            "error: " + application::format_application_start_error(application_result.error()));
        return 3;
    }

    // Keep the process alive until the operator requests a graceful shutdown.
    process_logger.log({logging::LogSeverity::info, "application", "SpareNode startup complete"});
    std::cout << "SpareNode " << version << " is running. Press Enter to stop.\n";
    std::string line;
    static_cast<void>(std::getline(std::cin, line));
    process_logger.log({logging::LogSeverity::info, "application", "SpareNode shutdown complete"});
    return 0;
}

} // namespace sparenode::app
