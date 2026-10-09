#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/config_parser.hpp"
#include "sparenode/configuration/config_validator.hpp"
#include "sparenode/configuration/runtime/location_permissions.hpp"
#include "sparenode/configuration/runtime_config_mapper.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Parses and validates complete configuration text required by mapper tests.
/// @param[in] input Configuration source retained throughout the pipeline.
/// @return Validated configuration accepted by the runtime mapping boundary.
[[nodiscard]] sparenode::configuration::ValidatedConfiguration
validate_configuration(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());
    auto parser_result =
        sparenode::configuration::ConfigParser::parse(std::move(lexer_result).value());
    REQUIRE(parser_result.has_value());
    auto validation_result =
        sparenode::configuration::ConfigValidator::validate(std::move(parser_result).value());
    REQUIRE(validation_result.has_value());
    return std::move(validation_result).value();
}

/// @brief Creates a minimal configuration containing one existing location root.
/// @param[in] path Existing directory exposed by the configuration.
/// @param[in] server_directives Optional server directives inserted before the location.
/// @param[in] location_directives Optional permission directives inserted after the path.
/// @return Complete syntactically and semantically valid source text.
[[nodiscard]] std::string make_configuration(const std::filesystem::path &path,
                                             const std::string_view server_directives = {},
                                             const std::string_view location_directives = {})
{
    return "server {\n" + std::string(server_directives) +
           "location \"/api/Documents\" {\npath \"" + path.generic_string() + "\";\n" +
           std::string(location_directives) + "}\n}";
}

} // namespace

TEST_CASE("Runtime configuration mapper applies every version one default",
          "[configuration][runtime]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-runtime-config");
    const auto validated = validate_configuration(make_configuration(directory.path()));

    const auto runtime_config = sparenode::configuration::RuntimeConfigMapper::map(validated);

    REQUIRE(runtime_config.servers().size() == 1);
    const auto &server = runtime_config.servers().front();
    CHECK(server.endpoint() == sparenode::network::TcpEndpoint{"127.0.0.1", 8080});
    CHECK_FALSE(server.multithreading_enabled());
    CHECK(server.worker_threads() == 1);
    CHECK(server.effective_worker_count() == 1);
    CHECK(server.minimum_log_severity() == sparenode::logging::LogSeverity::info);
    CHECK(server.http_timeouts().headers == std::chrono::seconds{10});
    CHECK(server.http_timeouts().body == std::chrono::seconds{10});
    CHECK(server.http_timeouts().total == std::chrono::seconds{30});
    CHECK(server.transport_policy().mode == sparenode::http::HttpTransportMode::loopback_http);
    CHECK_FALSE(server.transport_policy().trusted_proxy.has_value());
    REQUIRE(server.locations().size() == 1);
    CHECK(server.locations().front().api_path() == "/api/Documents");
    CHECK(std::filesystem::equivalent(server.locations().front().root().path(), directory.path()));
    CHECK(server.locations().front().permissions() ==
          sparenode::configuration::runtime::LocationPermissions{true, false, false});
}

TEST_CASE("Runtime configuration mapper preserves explicit validated settings",
          "[configuration][runtime]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-runtime-config");
    const std::string input =
        make_configuration(directory.path(),
                           "bind \"::1\";\nport 8443;\nmultithreading true;\nworker_threads 8;\n"
                           "log_level \"warning\";\nheader_timeout_ms 1500;\n"
                           "body_timeout_ms 2500;\nrequest_timeout_ms 90000;\n"
                           "transport_mode \"trusted_proxy_https\";\n"
                           "trusted_proxy \"::1\";\n",
                           "read false;\nwrite true;\ndelete true;\n");
    const auto validated = validate_configuration(input);

    const auto runtime_config = sparenode::configuration::RuntimeConfigMapper::map(validated);

    REQUIRE(runtime_config.servers().size() == 1);
    const auto &server = runtime_config.servers().front();
    CHECK(server.endpoint() == sparenode::network::TcpEndpoint{"::1", 8443});
    CHECK(server.multithreading_enabled());
    CHECK(server.worker_threads() == 8);
    CHECK(server.effective_worker_count() == 8);
    CHECK(server.minimum_log_severity() == sparenode::logging::LogSeverity::warning);
    CHECK(server.http_timeouts().headers == std::chrono::milliseconds{1500});
    CHECK(server.http_timeouts().body == std::chrono::milliseconds{2500});
    CHECK(server.http_timeouts().total == std::chrono::seconds{90});
    CHECK(server.transport_policy().mode ==
          sparenode::http::HttpTransportMode::trusted_proxy_https);
    CHECK(sparenode::test::require_optional(server.transport_policy().trusted_proxy) == "::1");
    REQUIRE(server.locations().size() == 1);
    CHECK(server.locations().front().permissions() ==
          sparenode::configuration::runtime::LocationPermissions{false, true, true});
}

TEST_CASE("Runtime configuration mapper transfers the validated MIME registry",
          "[configuration][runtime][mime-types]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-runtime-mime-config");
    const auto validated = validate_configuration(make_configuration(directory.path()));
    auto registry = sparenode::http::MimeTypeRegistry::create({{"png", "image/png"}});
    REQUIRE(registry.has_value());

    const auto runtime_config =
        sparenode::configuration::RuntimeConfigMapper::map(validated, std::move(registry).value());

    REQUIRE(runtime_config.servers().size() == 1);
    CHECK(runtime_config.servers().front().mime_types().content_type_for_path("file.png") ==
          "image/png");
}
