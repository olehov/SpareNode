#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "server.hpp"
#include "sparenode/application/command_line.hpp"
#include "sparenode/logging/console_log_sink.hpp"

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
    return sparenode::app::run_server(options.value());
}
