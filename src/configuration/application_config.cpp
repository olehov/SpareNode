#include "sparenode/configuration/application_config.hpp"

#include <filesystem>
#include <utility>

namespace sparenode::configuration
{

ApplicationConfig::ApplicationConfig(SharedRoot shared_root, const bool multithreading_enabled)
    : shared_root_(std::move(shared_root)), multithreading_enabled_(multithreading_enabled)
{
}

Result<ApplicationConfig, ApplicationConfigError>
ApplicationConfig::create(const EnvironmentFile &environment)
{
    bool multithreading_enabled = false;
    if (const auto *value = environment.find(multithreading_variable_name); value != nullptr)
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

    const auto *shared_root_value = environment.find(shared_root_variable_name);
    if (shared_root_value == nullptr)
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

    return ApplicationConfig(std::move(shared_root_result).value(), multithreading_enabled);
}

const SharedRoot &ApplicationConfig::shared_root() const noexcept
{
    return shared_root_;
}

bool ApplicationConfig::multithreading_enabled() const noexcept
{
    return multithreading_enabled_;
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
    }

    return "unknown application configuration error";
}

} // namespace sparenode::configuration
