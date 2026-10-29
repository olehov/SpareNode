#include "sparenode/bootstrap/runtime_bootstrap.hpp"

#include <chrono>
#include <memory>
#include <new>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

#include "sparenode/application/runtime/location_config.hpp"
#include "sparenode/application/runtime/server_config.hpp"
#include "sparenode/authentication/sodium_password_verifier.hpp"
#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/foundation/text/diagnostic_escape.hpp"
#include "sparenode/http/request/http_request_timeouts.hpp"
#include "sparenode/http/transport/http_transport_policy.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/session/sodium_session_credential_codec.hpp"

namespace sparenode::bootstrap
{
namespace
{

/// @brief Converts decoded UTF-8 path text into the host representation.
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string &value)
{
    const std::u8string utf8(value.begin(), value.end());
    return std::filesystem::path(utf8);
}

/// @brief Resolves a configured path relative to the main configuration file.
[[nodiscard]] std::filesystem::path resolve_path(const configuration::ConfiguredPath &configured,
                                                 const std::string_view configuration_path)
{
    auto path = path_from_utf8(configured.value);
    return path.is_absolute() ? std::move(path)
                              : std::filesystem::path(configuration_path).parent_path() / path;
}

/// @brief Converts a configuration-owned severity into the logging domain.
[[nodiscard]] logging::LogSeverity
map_log_level(const configuration::ConfiguredLogLevel level) noexcept
{
    switch (level)
    {
    case configuration::ConfiguredLogLevel::debug:
        return logging::LogSeverity::debug;
    case configuration::ConfiguredLogLevel::info:
        return logging::LogSeverity::info;
    case configuration::ConfiguredLogLevel::warning:
        return logging::LogSeverity::warning;
    case configuration::ConfiguredLogLevel::error:
        return logging::LogSeverity::error;
    }
    return logging::LogSeverity::error;
}

/// @brief Converts a configuration-owned transport mode into the HTTP domain.
[[nodiscard]] http::HttpTransportMode
map_transport_mode(const configuration::ConfiguredTransportMode mode) noexcept
{
    switch (mode)
    {
    case configuration::ConfiguredTransportMode::loopback_http:
        return http::HttpTransportMode::loopback_http;
    case configuration::ConfiguredTransportMode::trusted_lan_http:
        return http::HttpTransportMode::trusted_lan_http;
    case configuration::ConfiguredTransportMode::trusted_proxy_https:
        return http::HttpTransportMode::trusted_proxy_https;
    }
    return http::HttpTransportMode::loopback_http;
}

/// @brief Constructs concrete locations and records their roots for authentication checks.
[[nodiscard]] Result<std::vector<application::runtime::LocationConfig>, RuntimeBootstrapError>
build_locations(const configuration::ServerSettings &configured,
                const configuration::LoadedConfiguration &loaded,
                std::vector<filesystem::SharedRoot> &authentication_roots)
{
    std::vector<application::runtime::LocationConfig> locations;
    locations.reserve(configured.locations.size());
    for (const auto &location : configured.locations)
    {
        const auto root_path = resolve_path(location.root, loaded.source_path);
        auto root = filesystem::SharedRoot::create(root_path);
        if (!root)
        {
            return unexpected(
                RuntimeBootstrapError{RuntimeBootstrapErrorCode::invalid_location_root,
                                      loaded.source_path,
                                      location.root.location,
                                      root.error(),
                                      {},
                                      {}});
        }
        authentication_roots.push_back(root.value());
        locations.emplace_back(location.api_path, std::move(root).value(),
                               filesystem::LocationPermissions{location.allow_read,
                                                               location.allow_write,
                                                               location.allow_delete});
    }
    return locations;
}

/// @brief Loads an optional external MIME registry for one server.
[[nodiscard]] Result<http::MimeTypeRegistry, RuntimeBootstrapError>
load_mime_types(const configuration::ServerSettings &configured,
                const configuration::LoadedConfiguration &loaded)
{
    if (!configured.mime_types_file)
    {
        return http::MimeTypeRegistry{};
    }

    const auto mime_path = resolve_path(*configured.mime_types_file, loaded.source_path);
    auto mime_types = MimeTypesLoader::load(mime_path);
    if (!mime_types)
    {
        return unexpected(RuntimeBootstrapError{RuntimeBootstrapErrorCode::mime_types_load_failed,
                                                mime_path,
                                                configured.mime_types_file->location,
                                                {},
                                                mime_types.error(),
                                                {}});
    }
    return std::move(mime_types).value();
}

/// @brief Identifies the credential source and its diagnostic location.
struct CredentialSource
{
    std::filesystem::path path;               ///< Resolved credential file path.
    configuration::SourceLocation location{}; ///< Related configuration source location.
};

/// @brief Resolves the explicit or default credential file for one server.
[[nodiscard]] CredentialSource resolve_credentials(const configuration::ServerSettings &configured,
                                                   const configuration::LoadedConfiguration &loaded)
{
    if (configured.credentials_file)
    {
        return {resolve_path(*configured.credentials_file, loaded.source_path),
                configured.credentials_file->location};
    }
    return {std::filesystem::path(loaded.source_path).parent_path() / "credentials.conf",
            configured.location};
}

/// @brief Selects the file credential and libsodium adapters behind authentication core ports.
[[nodiscard]] Result<authentication::AuthenticationService,
                     authentication::FileAuthenticationErrorCode>
build_authentication(const std::filesystem::path &credentials_path,
                     std::span<const filesystem::SharedRoot> shared_roots)
{
    auto verifier = authentication::SodiumPasswordVerifier::create();
    if (!verifier)
    {
        return unexpected(authentication::FileAuthenticationErrorCode::cryptography_unavailable);
    }

    try
    {
        auto verifier_owner =
            std::make_shared<authentication::SodiumPasswordVerifier>(std::move(verifier).value());
        return authentication::create_file_authentication_service(credentials_path, shared_roots,
                                                                  std::move(verifier_owner));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(authentication::FileAuthenticationErrorCode::resource_exhausted);
    }
}

/// @brief Selects the libsodium credential codec behind the bounded session core.
[[nodiscard]] Result<std::shared_ptr<session::SessionStore>, RuntimeBootstrapErrorCode>
build_sessions()
{
    auto session_codec = session::SodiumSessionCredentialCodec::create();
    if (!session_codec)
    {
        return unexpected(RuntimeBootstrapErrorCode::session_initialization_failed);
    }
    auto codec_owner =
        std::make_shared<session::SodiumSessionCredentialCodec>(std::move(session_codec).value());
    auto sessions = session::SessionStore::create(std::move(codec_owner));
    if (!sessions)
    {
        return unexpected(RuntimeBootstrapErrorCode::session_initialization_failed);
    }
    return std::move(sessions).value();
}

/// @brief Joins credential verification to the selected bounded session implementation.
[[nodiscard]] Result<application::SessionAuthenticationService, RuntimeBootstrapErrorCode>
build_session_authentication(authentication::AuthenticationService authentication)
{
    auto sessions = build_sessions();
    if (!sessions)
    {
        return unexpected(sessions.error());
    }
    auto service = application::SessionAuthenticationService::create(std::move(authentication),
                                                                     std::move(sessions).value());
    if (!service)
    {
        return unexpected(RuntimeBootstrapErrorCode::session_initialization_failed);
    }
    return std::move(service).value();
}

} // namespace

/// @brief Stores concrete runtime settings and process-wide dependencies.
RuntimePlan::RuntimePlan(
    application::runtime::AppConfig config,
    std::shared_ptr<const application::SessionAuthenticationService> session_authentication,
    const logging::LogSeverity process_log_severity) noexcept
    : config_(std::move(config)), session_authentication_(std::move(session_authentication)),
      process_log_severity_(process_log_severity)
{
}

/// @brief Exposes the constructed application configuration.
const application::runtime::AppConfig &RuntimePlan::config() const noexcept
{
    return config_;
}

/// @brief Exposes the process-wide authentication service.
const authentication::AuthenticationService &RuntimePlan::authentication() const noexcept
{
    return session_authentication_->authentication();
}

const std::shared_ptr<const application::SessionAuthenticationService> &
RuntimePlan::session_authentication() const noexcept
{
    return session_authentication_;
}

const std::shared_ptr<session::SessionStore> &RuntimePlan::sessions() const noexcept
{
    return session_authentication_->sessions();
}

/// @brief Returns the threshold used for process lifecycle messages.
logging::LogSeverity RuntimePlan::process_log_severity() const noexcept
{
    return process_log_severity_;
}

/// @brief Converts validated configuration into concrete runtime dependencies.
Result<RuntimePlan, RuntimeBootstrapError>
RuntimeBootstrap::build(const configuration::LoadedConfiguration &loaded)
{
    try
    {
        std::vector<application::runtime::ServerConfig> runtime_servers;
        const auto &configured_servers = loaded.configuration.settings().servers;
        runtime_servers.reserve(configured_servers.size());
        std::vector<filesystem::SharedRoot> authentication_roots;
        std::filesystem::path credentials_path;
        configuration::SourceLocation credentials_location;
        std::optional<logging::LogSeverity> process_log_severity;

        for (const auto &configured : configured_servers)
        {
            auto locations = build_locations(configured, loaded, authentication_roots);
            if (!locations)
            {
                return unexpected(locations.error());
            }

            auto mime_types = load_mime_types(configured, loaded);
            if (!mime_types)
            {
                return unexpected(mime_types.error());
            }

            auto credentials = resolve_credentials(configured, loaded);
            credentials_path = std::move(credentials.path);
            credentials_location = credentials.location;

            const http::HttpRequestTimeouts timeouts{
                std::chrono::milliseconds{configured.header_timeout_ms},
                std::chrono::milliseconds{configured.body_timeout_ms},
                std::chrono::milliseconds{configured.request_timeout_ms}};
            http::HttpTransportPolicy transport_policy{
                map_transport_mode(configured.transport_mode), configured.trusted_proxy};
            const auto log_severity = map_log_level(configured.log_level);
            process_log_severity = process_log_severity.value_or(log_severity);
            runtime_servers.emplace_back(
                network::TcpEndpoint{configured.bind_address, configured.port},
                configured.multithreading_enabled, configured.worker_threads, log_severity,
                std::move(locations).value(), timeouts, std::move(mime_types).value(),
                std::move(transport_policy), credentials_path);
        }

        auto authentication = build_authentication(credentials_path, authentication_roots);
        if (!authentication)
        {
            return unexpected(
                RuntimeBootstrapError{RuntimeBootstrapErrorCode::authentication_failed,
                                      credentials_path,
                                      credentials_location,
                                      {},
                                      {},
                                      authentication.error()});
        }
        auto session_authentication =
            build_session_authentication(std::move(authentication).value());
        if (!session_authentication)
        {
            return unexpected(RuntimeBootstrapError{
                RuntimeBootstrapErrorCode::session_initialization_failed, {}, {}, {}, {}, {}});
        }
        auto shared_session_authentication =
            std::make_shared<const application::SessionAuthenticationService>(
                std::move(session_authentication).value());
        return RuntimePlan(application::runtime::AppConfig(std::move(runtime_servers)),
                           std::move(shared_session_authentication),
                           process_log_severity.value_or(logging::LogSeverity::info));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            RuntimeBootstrapError{RuntimeBootstrapErrorCode::resource_allocation_failed,
                                  loaded.source_path,
                                  {},
                                  {},
                                  {},
                                  {}});
    }
}

std::string format_runtime_bootstrap_error(const RuntimeBootstrapError &error)
{
    std::ostringstream output;
    if (error.mime_types_error)
    {
        text::append_safe_diagnostic(output, error.source_path.generic_string(),
                                     text::DiagnosticEscapeStyle::path);
        output << ':' << error.mime_types_error->line
               << ":1: error: " << to_string(error.mime_types_error->code);
        return output.str();
    }
    if (error.shared_root_error)
    {
        text::append_safe_diagnostic(output, error.source_path.generic_string(),
                                     text::DiagnosticEscapeStyle::path);
        output << ':' << error.location.line << ':' << error.location.column
               << ": error: " << filesystem::to_string(error.shared_root_error->code);
        return output.str();
    }
    if (error.authentication_error)
    {
        return std::string("error: ") + authentication::to_string(*error.authentication_error);
    }
    return std::string("error: ") + to_string(error.code);
}

const char *to_string(const RuntimeBootstrapErrorCode code) noexcept
{
    switch (code)
    {
    case RuntimeBootstrapErrorCode::invalid_location_root:
        return "configured filesystem location could not be constructed";
    case RuntimeBootstrapErrorCode::mime_types_load_failed:
        return "configured MIME registry could not be constructed";
    case RuntimeBootstrapErrorCode::authentication_failed:
        return "configured authentication service could not be constructed";
    case RuntimeBootstrapErrorCode::session_initialization_failed:
        return "bounded session service could not be constructed";
    case RuntimeBootstrapErrorCode::resource_allocation_failed:
        return "runtime bootstrap memory allocation failed";
    }
    return "unknown runtime bootstrap error";
}

} // namespace sparenode::bootstrap
