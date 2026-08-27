#include <catch2/catch_test_macros.hpp>

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

/// @brief Produces a quoted-path-safe configuration around one temporary share.
/// @param[in] path Existing or deliberately invalid host path.
/// @param[in] server_directives Optional server directives inserted before the share.
/// @param[in] share_directives Optional directives inserted after the required path.
/// @return Complete syntactically valid configuration source.
[[nodiscard]] std::string make_configuration(const std::filesystem::path &path,
                                             const std::string_view server_directives = {},
                                             const std::string_view share_directives = {})
{
    return "server {\n" + std::string(server_directives) + "share \"Documents\" {\npath \"" +
           path.generic_string() + "\";\n" + std::string(share_directives) + "}\n}";
}

} // namespace

TEST_CASE("Configuration validator accepts version one defaults and independent permissions",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");
    const std::string input =
        make_configuration(directory.path(), "bind \"::1\";\nport 443;\nlog_level \"warning\";\n",
                           "read false;\nwrite false;\ndelete true;\n");

    auto result = sparenode::configuration::ConfigValidator::validate(parse_configuration(input));

    REQUIRE(result.has_value());
    CHECK(result->parsed().server.shares.front().name == "Documents");
    REQUIRE(result->shared_roots().size() == 1);
    CHECK(result->shared_roots().front().path() == std::filesystem::canonical(directory.path()));
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

TEST_CASE("Configuration validator enforces version one share cardinality",
          "[configuration][validator]")
{
    SECTION("missing share")
    {
        const auto errors = require_validation_errors("server {}");
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().code == ConfigValidationErrorCode::missing_share);
        CHECK(errors.front().location == sparenode::configuration::SourceLocation{8, 1, 9});
    }

    SECTION("multiple shares remain individually validated")
    {
        constexpr std::string_view input = R"(server {
share "same" { }
share "same" { }
})";
        const auto errors = require_validation_errors(input);
        CHECK(contains_error(errors, ConfigValidationErrorCode::multiple_shares));
        CHECK(contains_error(errors, ConfigValidationErrorCode::duplicate_share_name));
        CHECK(contains_error(errors, ConfigValidationErrorCode::missing_share_path));
    }
}

TEST_CASE("Configuration validator reports share directive and filesystem failures",
          "[configuration][validator]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validator");

    SECTION("empty share name and missing path")
    {
        const auto errors = require_validation_errors("server { share \"\" {} }");
        CHECK(contains_error(errors, ConfigValidationErrorCode::empty_share_name));
        CHECK(contains_error(errors, ConfigValidationErrorCode::missing_share_path));
    }

    SECTION("duplicate path")
    {
        const std::string path = directory.path().generic_string();
        const auto errors = require_validation_errors("server { share \"Documents\" { path \"" +
                                                      path + "\"; path \"" + path + "\"; } }");
        CHECK(contains_error(errors, ConfigValidationErrorCode::duplicate_share_directive));
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
