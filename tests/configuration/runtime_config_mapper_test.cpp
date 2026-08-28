#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/config_parser.hpp"
#include "sparenode/configuration/config_validator.hpp"
#include "sparenode/configuration/runtime/share_permissions.hpp"
#include "sparenode/configuration/runtime_config_mapper.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
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

/// @brief Creates a minimal configuration containing one existing share root.
/// @param[in] path Existing directory exposed by the configuration.
/// @param[in] server_directives Optional server directives inserted before the share.
/// @param[in] share_directives Optional permission directives inserted after the path.
/// @return Complete syntactically and semantically valid source text.
[[nodiscard]] std::string make_configuration(const std::filesystem::path &path,
                                             const std::string_view server_directives = {},
                                             const std::string_view share_directives = {})
{
    return "server {\n" + std::string(server_directives) + "share \"Documents\" {\npath \"" +
           path.generic_string() + "\";\n" + std::string(share_directives) + "}\n}";
}

} // namespace

TEST_CASE("Runtime configuration mapper applies every version one default",
          "[configuration][runtime]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-runtime-config");
    const auto validated = validate_configuration(make_configuration(directory.path()));

    const auto runtime_config = sparenode::configuration::RuntimeConfigMapper::map(validated);

    REQUIRE(runtime_config.servers.size() == 1);
    const auto &server = runtime_config.servers.front();
    CHECK(server.endpoint == sparenode::network::TcpEndpoint{"0.0.0.0", 8080});
    CHECK_FALSE(server.multithreading_enabled);
    CHECK(server.worker_threads == 1);
    CHECK(server.effective_worker_count() == 1);
    CHECK(server.minimum_log_severity == sparenode::logging::LogSeverity::info);
    REQUIRE(server.shares.size() == 1);
    CHECK(server.shares.front().name == "Documents");
    CHECK(server.shares.front().root.path() == std::filesystem::canonical(directory.path()));
    CHECK(server.shares.front().permissions ==
          sparenode::configuration::runtime::SharePermissions{true, false, false});
}

TEST_CASE("Runtime configuration mapper preserves explicit validated settings",
          "[configuration][runtime]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-runtime-config");
    const std::string input =
        make_configuration(directory.path(),
                           "bind \"::1\";\nport 8443;\nmultithreading true;\nworker_threads 8;\n"
                           "log_level \"warning\";\n",
                           "read false;\nwrite true;\ndelete true;\n");
    const auto validated = validate_configuration(input);

    const auto runtime_config = sparenode::configuration::RuntimeConfigMapper::map(validated);

    REQUIRE(runtime_config.servers.size() == 1);
    const auto &server = runtime_config.servers.front();
    CHECK(server.endpoint == sparenode::network::TcpEndpoint{"::1", 8443});
    CHECK(server.multithreading_enabled);
    CHECK(server.worker_threads == 8);
    CHECK(server.effective_worker_count() == 8);
    CHECK(server.minimum_log_severity == sparenode::logging::LogSeverity::warning);
    REQUIRE(server.shares.size() == 1);
    CHECK(server.shares.front().permissions ==
          sparenode::configuration::runtime::SharePermissions{false, true, true});
}
