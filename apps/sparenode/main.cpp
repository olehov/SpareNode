#include <iostream>

#include "sparenode/configuration/application_config.hpp"
#include "sparenode/configuration/environment_file.hpp"
#include "sparenode/version.hpp"

namespace
{

/// @brief Writes a structured environment-file failure in a user-facing form.
/// @param[in] error File error returned by the universal environment loader.
void print_environment_file_error(const sparenode::configuration::EnvironmentFileError &error)
{
    std::cerr << "Error: " << sparenode::configuration::to_string(error.code) << " ["
              << error.source_path.string();
    if (error.line_number != 0)
    {
        std::cerr << ':' << error.line_number;
    }
    std::cerr << ']';
    if (!error.variable.empty())
    {
        std::cerr << ' ' << error.variable;
    }

    std::cerr << "\nCreate .env from .env.example and set SPARENODE_SHARED_ROOT.\n";
}

/// @brief Writes a semantic application-configuration failure for the user.
/// @param[in] error Error returned while interpreting parsed environment variables.
void print_application_config_error(const sparenode::configuration::ApplicationConfigError &error)
{
    std::cerr << "Error: " << sparenode::configuration::to_string(error.code);
    if (!error.variable.empty())
    {
        std::cerr << " [" << error.variable << ']';
    }
    if (error.shared_root_error)
    {
        std::cerr << ": " << sparenode::configuration::to_string(error.shared_root_error->code);
        if (error.shared_root_error->system_error)
        {
            std::cerr << " (" << error.shared_root_error->system_error.message() << ')';
        }
    }
    std::cerr << "\nUpdate SPARENODE_SHARED_ROOT in .env.\n";
}

} // namespace

/// @brief Loads `.env`, validates startup configuration, and reports the shared directory.
/// @return Zero on success, or two when the startup configuration is invalid.
int main()
{
    const auto environment_result = sparenode::configuration::EnvironmentFile::load_default();
    if (!environment_result)
    {
        print_environment_file_error(environment_result.error());
        return 2;
    }

    const auto configuration_result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());
    if (!configuration_result)
    {
        print_application_config_error(configuration_result.error());
        return 2;
    }

    std::cout << "SpareNode " << sparenode::version << '\n'
              << "Shared root: " << configuration_result->shared_root().path().string() << '\n';
    return 0;
}
