#include "sparenode/configuration/config_validator.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

#include "sparenode/http/http_request_timeouts.hpp"
#include "sparenode/logging/log_severity.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace sparenode::configuration
{
namespace
{

constexpr std::uint64_t minimum_port = 1;
constexpr std::uint64_t maximum_port = 65'535;
constexpr std::uint64_t minimum_worker_threads = 2;
constexpr std::uint64_t maximum_worker_threads = 64;
using directives::LocationDirectiveKind;
using directives::ParsedLocationDirective;
using directives::ParsedServerDirective;
using directives::ServerDirectiveKind;

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

/// @brief Reports whether one decoded location argument is a supported HTTP origin path.
[[nodiscard]] bool is_valid_location_path(const std::string_view path) noexcept
{
    if (path.empty() || path.front() != '/' || (path.size() > 1 && path.back() == '/'))
    {
        return false;
    }
    constexpr std::string_view punctuation = "-._~!$&'()+,;=:@";
    for (std::size_t index = 1; index < path.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(path[index]);
        const bool alpha_numeric = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                                   (byte >= 'a' && byte <= 'z');
        if (byte == '/' || alpha_numeric || punctuation.contains(static_cast<char>(byte)))
        {
            continue;
        }
        if (byte != '%' || path.size() - index < 3)
        {
            return false;
        }
        const auto hexadecimal = [](const unsigned char digit)
        {
            return (digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F') ||
                   (digit >= 'a' && digit <= 'f');
        };
        if (!hexadecimal(static_cast<unsigned char>(path[index + 1])) ||
            !hexadecimal(static_cast<unsigned char>(path[index + 2])))
        {
            return false;
        }
        index += 2;
    }
    if (path == "/")
    {
        return true;
    }
    std::size_t segment_begin = 1;
    while (segment_begin < path.size())
    {
        const auto separator = path.find('/', segment_begin);
        const auto segment = path.substr(segment_begin, separator - segment_begin);
        if (segment.empty() || segment == "." || segment == "..")
        {
            return false;
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        segment_begin = separator + 1;
    }
    return true;
}

/// @brief Reports whether two route prefixes overlap on a complete path-segment boundary.
[[nodiscard]] bool location_paths_conflict(const std::string_view left,
                                           const std::string_view right) noexcept
{
    const auto contains = [](const std::string_view parent, const std::string_view child)
    {
        return parent == child || parent == "/" ||
               (child.starts_with(parent) && child.size() > parent.size() &&
                child[parent.size()] == '/');
    };
    return contains(left, right) || contains(right, left);
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
        validate_locations();
        std::ranges::stable_sort(errors_, {}, [](const ConfigValidationError &error)
                                 { return error.location.byte_offset; });
        return std::move(errors_);
    }

    /// @brief Transfers canonical roots collected for successfully validated locations.
    /// @return Roots in the same order as the parsed location blocks.
    [[nodiscard]] std::vector<SharedRoot> release_location_roots() &&
    {
        return std::move(location_roots_);
    }

  private:
    /// @brief Validates singleton server directives and their individual values.
    void validate_server_directives()
    {
        std::unordered_set<ServerDirectiveKind> encountered;
        for (const auto &directive : configuration_.server.directives)
        {
            if (!encountered.insert(directive.kind).second)
            {
                add_server_error(ConfigValidationErrorCode::duplicate_server_directive, directive);
                continue;
            }
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
        case ServerDirectiveKind::header_timeout_ms:
        case ServerDirectiveKind::body_timeout_ms:
        case ServerDirectiveKind::request_timeout_ms:
            validate_timeout(directive);
            break;
        }
    }

    /// @brief Bounds a receive timeout before conversion to a signed chrono duration.
    /// @param[in] directive Integer timeout directive with a retained source location.
    void validate_timeout(const ParsedServerDirective &directive)
    {
        const auto value = std::get<std::uint64_t>(directive.value.scalar);
        if (value == 0 || value > static_cast<std::uint64_t>(
                                      http::HttpRequestTimeouts::maximum_config_timeout.count()))
        {
            add_server_error(ConfigValidationErrorCode::timeout_out_of_range, directive,
                             directive.value.location);
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

    /// @brief Requires at least one location and validates every supplied block.
    void validate_locations()
    {
        if (configuration_.server.locations.empty())
        {
            errors_.emplace_back(ConfigValidationErrorCode::missing_location,
                                 configuration_.server.closing_brace_location);
            return;
        }

        std::vector<std::string_view> paths;
        for (const auto &location : configuration_.server.locations)
        {
            validate_location_identity(location, paths);
            validate_location_directives(location);
        }
    }

    /// @brief Validates one API path and rejects duplicate or nested route prefixes.
    void validate_location_identity(const ParsedLocationBlock &location,
                                    std::vector<std::string_view> &paths)
    {
        if (!is_valid_location_path(location.api_path))
        {
            add_location_error(ConfigValidationErrorCode::invalid_location_path, location,
                               location.api_path_location);
            return;
        }
        if (std::ranges::any_of(paths, [&location](const std::string_view path)
                                { return location_paths_conflict(path, location.api_path); }))
        {
            add_location_error(ConfigValidationErrorCode::conflicting_location_path, location,
                               location.api_path_location);
            return;
        }
        paths.push_back(location.api_path);
    }

    /// @brief Validates location directive cardinality and its filesystem path.
    void validate_location_directives(const ParsedLocationBlock &location)
    {
        std::unordered_set<LocationDirectiveKind> encountered;
        const ParsedLocationDirective *path_directive = nullptr;
        for (const auto &directive : location.directives)
        {
            if (!encountered.insert(directive.kind).second)
            {
                add_location_directive_error(
                    ConfigValidationErrorCode::duplicate_location_directive, location, directive);
                continue;
            }
            if (directive.kind == LocationDirectiveKind::path)
            {
                path_directive = &directive;
            }
        }
        validate_location_root(location, path_directive);
    }

    /// @brief Requires a path and delegates canonical filesystem checks to SharedRoot.
    void validate_location_root(const ParsedLocationBlock &location,
                                const ParsedLocationDirective *path_directive)
    {
        if (path_directive == nullptr)
        {
            ConfigValidationError error{ConfigValidationErrorCode::missing_location_root,
                                        location.closing_brace_location};
            error.location_directive = LocationDirectiveKind::path;
            error.location_path = location.api_path;
            errors_.push_back(std::move(error));
            return;
        }

        const auto &path_text = std::get<std::string>(path_directive->value.scalar);
        auto root_result = SharedRoot::create(path_from_utf8(path_text));
        if (!root_result)
        {
            ConfigValidationError error{ConfigValidationErrorCode::invalid_location_root,
                                        path_directive->value.location};
            error.location_directive = LocationDirectiveKind::path;
            error.location_path = location.api_path;
            error.shared_root_error = std::move(root_result.error());
            errors_.push_back(std::move(error));
            return;
        }
        location_roots_.push_back(std::move(root_result).value());
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

    /// @brief Adds an error associated with one location block.
    void add_location_error(const ConfigValidationErrorCode code,
                            const ParsedLocationBlock &parsed_location,
                            const SourceLocation &source_location)
    {
        ConfigValidationError error{code, source_location};
        error.location_path = parsed_location.api_path;
        errors_.push_back(std::move(error));
    }

    /// @brief Adds an error associated with one location directive.
    void add_location_directive_error(const ConfigValidationErrorCode code,
                                      const ParsedLocationBlock &location,
                                      const ParsedLocationDirective &directive)
    {
        ConfigValidationError error{code, directive.location};
        error.location_directive = directive.kind;
        error.location_path = location.api_path;
        errors_.push_back(std::move(error));
    }

    const ParsedConfiguration &configuration_;
    std::vector<ConfigValidationError> errors_;
    std::vector<SharedRoot> location_roots_;
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

ValidatedConfiguration::ValidatedConfiguration(ParsedConfiguration parsed_configuration,
                                               std::vector<SharedRoot> location_roots)
    : parsed_(std::move(parsed_configuration)), location_roots_(std::move(location_roots))
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

const std::vector<SharedRoot> &ValidatedConfiguration::location_roots() const noexcept
{
    return location_roots_;
}

std::vector<SharedRoot> ValidatedConfiguration::release_location_roots() && noexcept
{
    return std::move(location_roots_);
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
    return ValidatedConfiguration(std::move(configuration),
                                  std::move(validation).release_location_roots());
}

/// @brief Describes a validation failure, including the accepted receive-timeout range.
/// @return Static diagnostic text suitable for configuration error reporting.
const char *to_string(const ConfigValidationErrorCode code) noexcept
{
    switch (code)
    {
    case ConfigValidationErrorCode::duplicate_server_directive:
        return "server directive is repeated";
    case ConfigValidationErrorCode::missing_location:
        return "at least one filesystem location is required";
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
    case ConfigValidationErrorCode::timeout_out_of_range:
        return "HTTP receive timeout must be between 1 and 86400000 milliseconds";
    case ConfigValidationErrorCode::invalid_location_path:
        return "location must be a supported absolute HTTP path";
    case ConfigValidationErrorCode::conflicting_location_path:
        return "location path conflicts with another filesystem location";
    case ConfigValidationErrorCode::duplicate_location_directive:
        return "location directive is repeated";
    case ConfigValidationErrorCode::missing_location_root:
        return "the required location filesystem path is missing";
    case ConfigValidationErrorCode::invalid_location_root:
        return "location filesystem path was rejected as a shared root";
    }
    return "unknown configuration validation error";
}

} // namespace sparenode::configuration
