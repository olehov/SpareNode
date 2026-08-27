#include "sparenode/configuration/config_validator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

#include "sparenode/logging/log_severity.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace sparenode::configuration
{
namespace
{

constexpr std::uint64_t minimum_port = 1;
constexpr std::uint64_t maximum_port = 65'535;
constexpr std::uint64_t minimum_worker_threads = 2;
constexpr std::uint64_t maximum_worker_threads = 64;
constexpr std::size_t server_directive_count = 5;
constexpr std::size_t share_directive_count = 4;

using directives::ParsedServerDirective;
using directives::ParsedShareDirective;
using directives::ServerDirectiveKind;
using directives::ShareDirectiveKind;

/// @brief Reports whether text is exactly one numeric IPv4 or IPv6 address.
[[nodiscard]] bool is_numeric_ip_address(const std::string &value) noexcept
{
    in_addr ipv4{};
    in6_addr ipv6{};
#ifdef _WIN32
    return InetPtonA(AF_INET, value.c_str(), &ipv4) == 1 ||
           InetPtonA(AF_INET6, value.c_str(), &ipv6) == 1;
#else
    return inet_pton(AF_INET, value.c_str(), &ipv4) == 1 ||
           inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
#endif
}

/// @brief Converts one decoded UTF-8 path value into the host filesystem representation.
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string &value)
{
    const std::u8string utf8_value(value.begin(), value.end());
    return std::filesystem::path(utf8_value);
}

/// @brief Collects semantic failures while retaining the original parser model.
class ValidationState final
{
  public:
    /// @brief Creates validation state for one complete parser result.
    /// @param[in] configuration Parsed model retained for the duration of validation.
    explicit ValidationState(const ParsedConfiguration &configuration)
        : configuration_(configuration)
    {
    }

    /// @brief Applies every independently detectable version-one validation rule.
    /// @return Collected errors in deterministic source traversal order.
    [[nodiscard]] std::vector<ConfigValidationError> validate()
    {
        validate_server_directives();
        validate_shares();
        std::ranges::stable_sort(errors_, {}, [](const ConfigValidationError &error)
                                 { return error.location.byte_offset; });
        return std::move(errors_);
    }

  private:
    /// @brief Validates singleton server directives and their individual values.
    void validate_server_directives()
    {
        std::array<bool, server_directive_count> encountered{};
        for (const auto &directive : configuration_.server.directives)
        {
            const auto index = static_cast<std::size_t>(directive.kind);
            if (encountered[index])
            {
                add_server_error(ConfigValidationErrorCode::duplicate_server_directive, directive);
                continue;
            }
            encountered[index] = true;
            validate_server_directive(directive);
        }
        validate_threading_relationship();
    }

    /// @brief Dispatches one server directive to its semantic rule.
    void validate_server_directive(const ParsedServerDirective &directive)
    {
        switch (directive.kind)
        {
        case ServerDirectiveKind::bind:
            validate_bind(directive);
            break;
        case ServerDirectiveKind::port:
            validate_port(directive);
            break;
        case ServerDirectiveKind::multithreading:
            multithreading_enabled_ = std::get<bool>(directive.value.scalar);
            break;
        case ServerDirectiveKind::worker_threads:
            worker_threads_ = std::get<std::uint64_t>(directive.value.scalar);
            worker_threads_directive_ = &directive;
            break;
        case ServerDirectiveKind::log_level:
            validate_log_level(directive);
            break;
        }
    }

    /// @brief Rejects host names and malformed numeric address strings.
    void validate_bind(const ParsedServerDirective &directive)
    {
        const auto &address = std::get<std::string>(directive.value.scalar);
        if (!is_numeric_ip_address(address))
        {
            add_server_error(ConfigValidationErrorCode::invalid_bind_address, directive,
                             directive.value.location);
        }
    }

    /// @brief Enforces the non-zero TCP port range accepted by configuration.
    void validate_port(const ParsedServerDirective &directive)
    {
        const std::uint64_t port = std::get<std::uint64_t>(directive.value.scalar);
        if (port < minimum_port || port > maximum_port)
        {
            add_server_error(ConfigValidationErrorCode::port_out_of_range, directive,
                             directive.value.location);
        }
    }

    /// @brief Accepts only severities documented by the persistent format.
    void validate_log_level(const ParsedServerDirective &directive)
    {
        const auto &value = std::get<std::string>(directive.value.scalar);
        if (!logging::parse_log_severity(value).has_value())
        {
            add_server_error(ConfigValidationErrorCode::invalid_log_level, directive,
                             directive.value.location);
        }
    }

    /// @brief Enforces the dependency between multithreading and worker count.
    void validate_threading_relationship()
    {
        if (multithreading_enabled_)
        {
            validate_enabled_threading();
            return;
        }
        if (worker_threads_directive_ != nullptr)
        {
            add_server_error(ConfigValidationErrorCode::unexpected_worker_threads,
                             *worker_threads_directive_);
        }
    }

    /// @brief Requires and bounds the worker count while multithreading is enabled.
    void validate_enabled_threading()
    {
        if (worker_threads_directive_ == nullptr)
        {
            ConfigValidationError error{ConfigValidationErrorCode::missing_worker_threads,
                                        configuration_.server.closing_brace_location};
            error.server_directive = ServerDirectiveKind::worker_threads;
            errors_.push_back(std::move(error));
            return;
        }
        if (worker_threads_ < minimum_worker_threads || worker_threads_ > maximum_worker_threads)
        {
            add_server_error(ConfigValidationErrorCode::worker_threads_out_of_range,
                             *worker_threads_directive_, worker_threads_directive_->value.location);
        }
    }

    /// @brief Enforces share cardinality and validates every supplied share independently.
    void validate_shares()
    {
        if (configuration_.server.shares.empty())
        {
            errors_.emplace_back(ConfigValidationErrorCode::missing_share,
                                 configuration_.server.closing_brace_location);
            return;
        }
        if (configuration_.server.shares.size() > 1)
        {
            errors_.emplace_back(ConfigValidationErrorCode::multiple_shares,
                                 configuration_.server.shares[1].location);
        }

        std::unordered_set<std::string> share_names;
        for (const auto &share : configuration_.server.shares)
        {
            validate_share_identity(share, share_names);
            validate_share_directives(share);
        }
    }

    /// @brief Validates one share name and detects an exact duplicate.
    void validate_share_identity(const ParsedShareBlock &share,
                                 std::unordered_set<std::string> &share_names)
    {
        if (share.name.empty())
        {
            add_share_error(ConfigValidationErrorCode::empty_share_name, share,
                            share.name_location);
        }
        if (!share_names.insert(share.name).second)
        {
            add_share_error(ConfigValidationErrorCode::duplicate_share_name, share,
                            share.name_location);
        }
    }

    /// @brief Validates share directive cardinality and the first path value.
    void validate_share_directives(const ParsedShareBlock &share)
    {
        std::array<bool, share_directive_count> encountered{};
        const ParsedShareDirective *path_directive = nullptr;
        for (const auto &directive : share.directives)
        {
            const auto index = static_cast<std::size_t>(directive.kind);
            if (encountered[index])
            {
                add_share_directive_error(ConfigValidationErrorCode::duplicate_share_directive,
                                          share, directive);
                continue;
            }
            encountered[index] = true;
            if (directive.kind == ShareDirectiveKind::path)
            {
                path_directive = &directive;
            }
        }
        validate_share_path(share, path_directive);
    }

    /// @brief Requires a path and delegates canonical filesystem checks to SharedRoot.
    void validate_share_path(const ParsedShareBlock &share,
                             const ParsedShareDirective *path_directive)
    {
        if (path_directive == nullptr)
        {
            ConfigValidationError error{ConfigValidationErrorCode::missing_share_path,
                                        share.closing_brace_location};
            error.share_directive = ShareDirectiveKind::path;
            error.share_name = share.name;
            errors_.push_back(std::move(error));
            return;
        }

        const auto &path_text = std::get<std::string>(path_directive->value.scalar);
        auto root_result = SharedRoot::create(path_from_utf8(path_text));
        if (!root_result)
        {
            ConfigValidationError error{ConfigValidationErrorCode::invalid_share_path,
                                        path_directive->value.location};
            error.share_directive = ShareDirectiveKind::path;
            error.share_name = share.name;
            error.shared_root_error = std::move(root_result.error());
            errors_.push_back(std::move(error));
        }
    }

    /// @brief Adds an error associated with one server directive.
    void add_server_error(const ConfigValidationErrorCode code,
                          const ParsedServerDirective &directive)
    {
        add_server_error(code, directive, directive.location);
    }

    /// @brief Adds an error associated with one server directive at a selected token.
    void add_server_error(const ConfigValidationErrorCode code,
                          const ParsedServerDirective &directive, const SourceLocation &location)
    {
        ConfigValidationError error{code, location};
        error.server_directive = directive.kind;
        errors_.push_back(std::move(error));
    }

    /// @brief Adds an error associated with one share block.
    void add_share_error(const ConfigValidationErrorCode code, const ParsedShareBlock &share,
                         const SourceLocation &location)
    {
        ConfigValidationError error{code, location};
        error.share_name = share.name;
        errors_.push_back(std::move(error));
    }

    /// @brief Adds an error associated with one share directive.
    void add_share_directive_error(const ConfigValidationErrorCode code,
                                   const ParsedShareBlock &share,
                                   const ParsedShareDirective &directive)
    {
        ConfigValidationError error{code, directive.location};
        error.share_directive = directive.kind;
        error.share_name = share.name;
        errors_.push_back(std::move(error));
    }

    const ParsedConfiguration &configuration_;
    std::vector<ConfigValidationError> errors_;
    bool multithreading_enabled_{false};
    std::uint64_t worker_threads_{};
    const ParsedServerDirective *worker_threads_directive_{};
};

} // namespace

ConfigValidationError::ConfigValidationError(const ConfigValidationErrorCode error_code,
                                             const SourceLocation &source_location)
    : code(error_code), location(source_location)
{
}

ValidatedConfiguration::ValidatedConfiguration(ParsedConfiguration parsed_configuration)
    : parsed_(std::move(parsed_configuration))
{
}

const ParsedConfiguration &ValidatedConfiguration::parsed() const noexcept
{
    return parsed_;
}

ParsedConfiguration ValidatedConfiguration::release_parsed() && noexcept
{
    return std::move(parsed_);
}

Result<ValidatedConfiguration, std::vector<ConfigValidationError>>
ConfigValidator::validate(ParsedConfiguration configuration)
{
    ValidationState validation(configuration);
    auto errors = validation.validate();
    if (!errors.empty())
    {
        return unexpected(std::move(errors));
    }
    return ValidatedConfiguration(std::move(configuration));
}

const char *to_string(const ConfigValidationErrorCode code) noexcept
{
    switch (code)
    {
    case ConfigValidationErrorCode::duplicate_server_directive:
        return "server directive is repeated";
    case ConfigValidationErrorCode::missing_share:
        return "the required share block is missing";
    case ConfigValidationErrorCode::multiple_shares:
        return "version one permits exactly one share block";
    case ConfigValidationErrorCode::invalid_bind_address:
        return "bind must be a numeric IPv4 or IPv6 address";
    case ConfigValidationErrorCode::port_out_of_range:
        return "port must be between 1 and 65535";
    case ConfigValidationErrorCode::missing_worker_threads:
        return "worker_threads is required when multithreading is enabled";
    case ConfigValidationErrorCode::unexpected_worker_threads:
        return "worker_threads requires multithreading to be enabled";
    case ConfigValidationErrorCode::worker_threads_out_of_range:
        return "worker_threads must be between 2 and 64";
    case ConfigValidationErrorCode::invalid_log_level:
        return "log_level is not supported";
    case ConfigValidationErrorCode::empty_share_name:
        return "share name must not be empty";
    case ConfigValidationErrorCode::duplicate_share_name:
        return "share name is repeated";
    case ConfigValidationErrorCode::duplicate_share_directive:
        return "share directive is repeated";
    case ConfigValidationErrorCode::missing_share_path:
        return "the required share path is missing";
    case ConfigValidationErrorCode::invalid_share_path:
        return "share path is not an existing directory";
    }
    return "unknown configuration validation error";
}

} // namespace sparenode::configuration
