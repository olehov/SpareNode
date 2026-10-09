#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/config_parser.hpp"
#include "sparenode/configuration/config_validator.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

using sparenode::configuration::ConfigValidationError;
using sparenode::configuration::ConfigValidationErrorCode;
using sparenode::configuration::ParsedConfiguration;

/// @brief Parses complete test input and requires both syntax stages to succeed.
/// @param[in] input Configuration source retained throughout parsing.
/// @return Owned parser model ready for semantic validation.
[[nodiscard]] ParsedConfiguration parse_configuration(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());
    auto parser_result =
        sparenode::configuration::ConfigParser::parse(std::move(lexer_result).value());
    REQUIRE(parser_result.has_value());
    return std::move(parser_result).value();
}

/// @brief Validates malformed semantic input and requires at least one failure.
/// @param[in] input Syntactically valid configuration source.
/// @return Complete semantic error collection.
[[nodiscard]] std::vector<ConfigValidationError>
require_validation_errors(const std::string_view input)
{
    auto validation_result =
        sparenode::configuration::ConfigValidator::validate(parse_configuration(input));
    REQUIRE(!validation_result.has_value());
    return std::move(validation_result.error());
}

/// @brief Reports whether a collected validation result contains one category.
/// @param[in] errors Semantic errors to search.
/// @param[in] code Stable category expected by the test.
/// @return `true` when at least one matching error exists.
[[nodiscard]] bool contains_error(const std::vector<ConfigValidationError> &errors,
                                  const ConfigValidationErrorCode code)
{
    return std::ranges::any_of(errors, [code](const auto &error) { return error.code == code; });
}

/// @brief Produces a quoted-path-safe configuration around one temporary location.
/// @param[in] path Existing or deliberately invalid host path.
/// @param[in] server_directives Optional server directives inserted before the location.
/// @param[in] location_directives Optional directives inserted after the required path.
/// @return Complete syntactically valid configuration source.
[[nodiscard]] std::string make_configuration(const std::filesystem::path &path,
                                             const std::string_view server_directives = {},
                                             const std::string_view location_directives = {})
{
    return "server {\n" + std::string(server_directives) +
           "location \"/api/Documents\" {\npath \"" + path.generic_string() + "\";\n" +
           std::string(location_directives) + "}\n}";
}

} // namespace

TEST_CASE("Configuration timeout directives enforce bounds and singleton semantics",
          "[configuration][validator][timeout]")
{
    const std::string directive =
        GENERATE("header_timeout_ms", "body_timeout_ms", "request_timeout_ms");
    const sparenode::test::TemporaryDirectory directory("sparenode-timeout-validator");
    SECTION("inclusive valid boundaries")
    {
        const std::string value = GENERATE("1", "86400000");
        const auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(
            make_configuration(directory.path(), directive + " " + value + ";")));
        REQUIRE(result.has_value());
    }
    SECTION("zero overflow and excessive durations")
    {
        const std::string value = GENERATE("0", "86400001", "18446744073709551615");
        const auto errors = require_validation_errors(
            make_configuration(directory.path(), directive + " " + value + ";"));
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::timeout_out_of_range);
        CHECK(errors.front().server_directive.has_value());
    }
    SECTION("duplicates")
    {
        const auto errors = require_validation_errors(
            make_configuration(directory.path(), directive + " 1000; " + directive + " 2000;"));
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::duplicate_server_directive);
    }
}

TEST_CASE("Configuration validator accepts version one defaults and independent permissions",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");
    const std::string input =
        make_configuration(directory.path(), "bind \"::1\";\nport 443;\nlog_level \"warning\";\n",
                           "read false;\nwrite false;\ndelete true;\n");

    auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(input));

    REQUIRE(result.has_value());
    CHECK(result->parsed().server.locations.front().api_path == "/api/Documents");
    REQUIRE(result->location_roots().size() == 1);
    CHECK(std::filesystem::equivalent(result->location_roots().front().path(), directory.path()));
}

TEST_CASE("Configuration validator requires a nonempty singleton MIME file path",
          "[configuration][validator][mime-types]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-mime-validator");

    const auto accepted = sparenode::configuration::ConfigValidator::validate(parse_configuration(
        make_configuration(directory.path(), "mime_types_file \"mime.types\";\n")));
    REQUIRE(accepted.has_value());

    const auto empty =
        require_validation_errors(make_configuration(directory.path(), "mime_types_file \"\";\n"));
    REQUIRE(empty.size() == 1);
    CHECK(empty.front().code == ConfigValidationErrorCode::invalid_mime_types_file);

    const auto duplicate = require_validation_errors(make_configuration(
        directory.path(), "mime_types_file \"first.types\";\nmime_types_file \"second.types\";\n"));
    REQUIRE(duplicate.size() == 1);
    CHECK(duplicate.front().code == ConfigValidationErrorCode::duplicate_server_directive);
}

TEST_CASE("Configuration validator collects independent server failures",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");
    const std::string input = make_configuration(
        directory.path(), "bind \"localhost\";\nport 0;\nport 8080;\nmultithreading true;\n"
                          "worker_threads 65;\nlog_level \"trace\";\n");

    const auto errors = require_validation_errors(input);

    CHECK(std::ranges::is_sorted(errors, {}, [](const ConfigValidationError &error)
                                 { return error.location.byte_offset; }));
    CHECK(contains_error(errors, ConfigValidationErrorCode::invalid_bind_address));
    CHECK(contains_error(errors, ConfigValidationErrorCode::port_out_of_range));
    CHECK(contains_error(errors, ConfigValidationErrorCode::duplicate_server_directive));
    CHECK(contains_error(errors, ConfigValidationErrorCode::worker_threads_out_of_range));
    CHECK(contains_error(errors, ConfigValidationErrorCode::invalid_log_level));
}

TEST_CASE("Configuration validator enforces worker thread relationships",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");

    SECTION("enabled without a worker count")
    {
        const auto errors = require_validation_errors(
            make_configuration(directory.path(), "multithreading true;\n"));
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::missing_worker_threads);
    }

    SECTION("disabled with a worker count")
    {
        const auto errors =
            require_validation_errors(make_configuration(directory.path(), "worker_threads 4;\n"));
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::unexpected_worker_threads);
    }

    SECTION("enabled with the lower worker boundary")
    {
        auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(
            make_configuration(directory.path(), "multithreading true;\nworker_threads 2;\n")));
        CHECK(result.has_value());
    }

    SECTION("enabled with the upper worker boundary")
    {
        auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(
            make_configuration(directory.path(), "multithreading true;\nworker_threads 64;\n")));
        CHECK(result.has_value());
    }

    SECTION("explicitly disabled with a worker count")
    {
        const auto errors = require_validation_errors(
            make_configuration(directory.path(), "multithreading false;\nworker_threads 4;\n"));
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::unexpected_worker_threads);
    }
}

TEST_CASE("Configuration validator enforces authentication transport boundaries",
          "[configuration][validator][transport]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-transport-validator");

    SECTION("safe default and explicit loopback addresses")
    {
        auto defaults = sparenode::configuration::ConfigValidator::validate(
            parse_configuration(make_configuration(directory.path())));
        CHECK(defaults.has_value());

        auto ipv4_range = sparenode::configuration::ConfigValidator::validate(
            parse_configuration(make_configuration(directory.path(), "bind \"127.20.30.40\";\n")));
        CHECK(ipv4_range.has_value());

        auto ipv6 = sparenode::configuration::ConfigValidator::validate(
            parse_configuration(make_configuration(directory.path(), "bind \"::1\";\n")));
        CHECK(ipv6.has_value());
    }

    SECTION("loopback mode rejects network-facing listeners")
    {
        const auto errors =
            require_validation_errors(make_configuration(directory.path(), "bind \"0.0.0.0\";\n"));
        CHECK(contains_error(errors, ConfigValidationErrorCode::unsafe_transport_bind));
    }

    SECTION("trusted LAN mode makes plaintext network access explicit")
    {
        auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(
            make_configuration(directory.path(), "bind \"0.0.0.0\";\n"
                                                 "transport_mode \"trusted_lan_http\";\n")));
        CHECK(result.has_value());
    }

    SECTION("trusted proxy mode requires one numeric proxy and a restricted listener")
    {
        auto accepted = sparenode::configuration::ConfigValidator::validate(parse_configuration(
            make_configuration(directory.path(), "bind \"192.0.2.10\";\n"
                                                 "transport_mode \"trusted_proxy_https\";\n"
                                                 "trusted_proxy \"192.0.2.1\";\n")));
        CHECK(accepted.has_value());

        const auto missing = require_validation_errors(
            make_configuration(directory.path(), "transport_mode \"trusted_proxy_https\";\n"));
        CHECK(contains_error(missing, ConfigValidationErrorCode::missing_trusted_proxy));

        const auto invalid = require_validation_errors(
            make_configuration(directory.path(), "transport_mode \"trusted_proxy_https\";\n"
                                                 "trusted_proxy \"proxy.internal\";\n"));
        CHECK(contains_error(invalid, ConfigValidationErrorCode::invalid_trusted_proxy));

        const auto wildcard = require_validation_errors(make_configuration(
            directory.path(), "bind \"::\";\ntransport_mode \"trusted_proxy_https\";\n"
                              "trusted_proxy \"::1\";\n"));
        CHECK(contains_error(wildcard, ConfigValidationErrorCode::unsafe_transport_bind));
    }

    SECTION("trusted proxy is rejected in direct modes")
    {
        const auto loopback = require_validation_errors(
            make_configuration(directory.path(), "trusted_proxy \"127.0.0.1\";\n"));
        CHECK(contains_error(loopback, ConfigValidationErrorCode::unexpected_trusted_proxy));

        const auto lan = require_validation_errors(
            make_configuration(directory.path(), "transport_mode \"trusted_lan_http\";\n"
                                                 "trusted_proxy \"192.0.2.1\";\n"));
        CHECK(contains_error(lan, ConfigValidationErrorCode::unexpected_trusted_proxy));
    }

    SECTION("unsupported and duplicate transport directives are rejected")
    {
        const auto unsupported = require_validation_errors(
            make_configuration(directory.path(), "transport_mode \"public_http\";\n"));
        CHECK(contains_error(unsupported, ConfigValidationErrorCode::invalid_transport_mode));

        const auto duplicate = require_validation_errors(
            make_configuration(directory.path(), "transport_mode \"loopback_http\";\n"
                                                 "transport_mode \"trusted_lan_http\";\n"));
        CHECK(contains_error(duplicate, ConfigValidationErrorCode::duplicate_server_directive));
    }
}

TEST_CASE("Configuration validator requires locations and rejects ambiguous paths",
          "[configuration][validator]")
{
    SECTION("missing location")
    {
        const auto errors = require_validation_errors("server {}");
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::missing_location);
        CHECK(errors.front().location == sparenode::configuration::SourceLocation{8, 1, 9});
    }

    const sparenode::test::TemporaryDirectory directory("sparenode-location-validator");
    const auto root = directory.path().generic_string();

    SECTION("multiple distinct locations are accepted")
    {
        const auto input = "server { location \"/api/docs\" { path \"" + root +
                           "\"; } location \"/api/media\" { path \"" + root + "\"; } }";
        const auto result =
            sparenode::configuration::ConfigValidator::validate(parse_configuration(input));
        REQUIRE(result);
        CHECK(result->parsed().server.locations.size() == 2);
    }

    SECTION("duplicate and nested paths conflict")
    {
        const std::string second_path = GENERATE("/api/docs", "/api/docs/private");
        const auto input = "server { location \"/api/docs\" { path \"" + root +
                           "\"; } location \"" + second_path + "\" { path \"" + root + "\"; } }";
        const auto errors = require_validation_errors(input);
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::conflicting_location_path);
    }

    SECTION("segment neighbors do not conflict")
    {
        const auto input = "server { location \"/api/docs\" { path \"" + root +
                           "\"; } location \"/api/docs-old\" { path \"" + root + "\"; } }";
        const auto result =
            sparenode::configuration::ConfigValidator::validate(parse_configuration(input));
        REQUIRE(result);
    }
}

TEST_CASE("Configuration validator reports location directive and filesystem failures",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");

    SECTION("invalid API path and missing filesystem path")
    {
        const auto errors = require_validation_errors("server { location \"\" {} }");
        CHECK(contains_error(errors, ConfigValidationErrorCode::invalid_location_path));
        CHECK(contains_error(errors, ConfigValidationErrorCode::missing_location_root));
    }

    SECTION("malformed API paths")
    {
        const std::string api_path =
            GENERATE("relative", "/trailing/", "/double//segment", "/dot/../segment",
                     "/query?value", "/fragment#value", "/wildcard/*", "/back\\\\slash");
        const auto errors =
            require_validation_errors("server { location \"" + api_path + "\" { path \"" +
                                      directory.path().generic_string() + "\"; } }");
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::invalid_location_path);
    }

    SECTION("duplicate path")
    {
        const std::string path = directory.path().generic_string();
        const auto errors =
            require_validation_errors("server { location \"/api/Documents\" { path \"" + path +
                                      "\"; path \"" + path + "\"; } }");
        CHECK(contains_error(errors, ConfigValidationErrorCode::duplicate_location_directive));
    }

    SECTION("missing filesystem entry")
    {
        const auto errors =
            require_validation_errors(make_configuration(directory.path() / "missing"));
        REQUIRE(errors.size() == 1);
        REQUIRE(errors.front().shared_root_error.has_value());
        CHECK(sparenode::test::require_optional(errors.front().shared_root_error).code ==
              sparenode::configuration::SharedRootErrorCode::not_found);
    }

    SECTION("regular file")
    {
        const auto file = directory.path() / "file.txt";
        std::ofstream output(file);
        output << "not a directory";
        output.close();

        const auto errors = require_validation_errors(make_configuration(file));
        REQUIRE(errors.size() == 1);
        REQUIRE(errors.front().shared_root_error.has_value());
        CHECK(sparenode::test::require_optional(errors.front().shared_root_error).code ==
              sparenode::configuration::SharedRootErrorCode::not_directory);
    }
}

TEST_CASE("Configuration validation error descriptions are stable", "[configuration][validator]")
{
    CHECK(std::string_view(sparenode::configuration::to_string(
              ConfigValidationErrorCode::invalid_bind_address)) ==
          "bind must be a numeric IPv4 or IPv6 address");
}
