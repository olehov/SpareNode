#include "sparenode/configuration/application_config.hpp"

#include <filesystem>
#include <utility>

namespace sparenode::configuration
{

ApplicationConfig::ApplicationConfig(SharedRoot shared_root, const bool multithreading_enabled,
                                     const logging::LogSeverity minimum_log_severity)
    : shared_root_(std::move(shared_root)), multithreading_enabled_(multithreading_enabled),
      minimum_log_severity_(minimum_log_severity)
{
}

Result<ApplicationConfig, ApplicationConfigError>
ApplicationConfig::create(const EnvironmentFile &environment)
{
    logging::LogSeverity minimum_log_severity = logging::LogSeverity::info;
    if (const auto value = environment.find(log_level_variable_name); value.has_value())
    {
        const auto parsed_severity = logging::parse_log_severity(*value);
        if (!parsed_severity.has_value())
        {
            return unexpected(
                ApplicationConfigError{ApplicationConfigErrorCode::invalid_log_severity,
                                       std::string(log_level_variable_name), std::nullopt});
        }
        minimum_log_severity = parsed_severity.value();
    }

    bool multithreading_enabled = false;
    if (const auto value = environment.find(multithreading_variable_name); value.has_value())
    {
        if (value->empty())
        {
            return unexpected(ApplicationConfigError{ApplicationConfigErrorCode::invalid_boolean,
                                                     std::string(multithreading_variable_name),
                                                     std::nullopt});
        }
        if (*value == "true")
        {
            multithreading_enabled = true;
        }
        else if (*value == "false")
        {
            multithreading_enabled = false;
        }
        else
        {
            return unexpected(ApplicationConfigError{ApplicationConfigErrorCode::invalid_boolean,
                                                     std::string(multithreading_variable_name),
                                                     std::nullopt});
        }
    }

    const auto shared_root_value = environment.find(shared_root_variable_name);
    if (!shared_root_value.has_value())
    {
        return unexpected(ApplicationConfigError{ApplicationConfigErrorCode::missing_shared_root,
                                                 std::string(shared_root_variable_name),
                                                 std::nullopt});
    }
    if (shared_root_value->empty())
    {
        return unexpected(ApplicationConfigError{ApplicationConfigErrorCode::missing_value,
                                                 std::string(shared_root_variable_name),
                                                 std::nullopt});
    }

    const std::u8string shared_root_utf8(shared_root_value->begin(), shared_root_value->end());
    auto shared_root_result = SharedRoot::create(std::filesystem::path(shared_root_utf8));
    if (!shared_root_result)
    {
        return unexpected(ApplicationConfigError{ApplicationConfigErrorCode::invalid_shared_root,
                                                 std::string(shared_root_variable_name),
                                                 shared_root_result.error()});
    }

    return ApplicationConfig(std::move(shared_root_result).value(), multithreading_enabled,
                             minimum_log_severity);
}

const SharedRoot &ApplicationConfig::shared_root() const noexcept
{
    return shared_root_;
}

bool ApplicationConfig::multithreading_enabled() const noexcept
{
    return multithreading_enabled_;
}

logging::LogSeverity ApplicationConfig::minimum_log_severity() const noexcept
{
    return minimum_log_severity_;
}

const char *to_string(const ApplicationConfigErrorCode code) noexcept
{
    switch (code)
    {
    case ApplicationConfigErrorCode::missing_shared_root:
        return "SPARENODE_SHARED_ROOT is missing";
    case ApplicationConfigErrorCode::missing_value:
        return "SPARENODE_SHARED_ROOT is empty";
    case ApplicationConfigErrorCode::invalid_shared_root:
        return "SPARENODE_SHARED_ROOT does not identify a valid directory";
    case ApplicationConfigErrorCode::invalid_boolean:
        return "configuration boolean must be true or false";
    case ApplicationConfigErrorCode::invalid_log_severity:
        return "log severity must be debug, info, warning, or error";
    }

    return "unknown application configuration error";
}

} // namespace sparenode::configuration
