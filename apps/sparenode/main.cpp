#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/application/command_line.hpp"
#include "sparenode/application/running_application.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/logger.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"
#include "sparenode/version.hpp"

int main(const int argc, const char *const argv[])
{
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }

    const auto options = sparenode::application::parse_command_line(arguments);
    if (!options)
    {
        const std::string diagnostic =
            std::string("error: ") + sparenode::application::to_string(options.error().code);
        sparenode::logging::write_console_diagnostic(
            std::cerr, diagnostic, sparenode::logging::LogSeverity::error);
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

    const auto console_sink = std::make_shared<sparenode::logging::ConsoleLogSink>(std::clog);
    const auto minimum_severity = config_result->servers.front().minimum_log_severity;
    const sparenode::logging::Logger logger(console_sink, minimum_severity);

    auto handler = [](sparenode::network::TcpConnection, const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError> { return {}; };
    auto application_result = sparenode::application::RunningApplication::start(
        std::move(config_result).value(), std::move(handler));
    if (!application_result)
    {
        const std::string diagnostic = std::string("error: ") +
                                       sparenode::application::to_string(
                                           application_result.error().code);
        sparenode::logging::write_console_diagnostic(
            std::cerr, diagnostic, sparenode::logging::LogSeverity::error);
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
