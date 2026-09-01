#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

#include "sparenode/configuration/environment_file.hpp"
#include "support/environment_file.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Environment file preserves every parsed assignment", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "\xEF\xBB\xBF# configuration\nFIRST=value\nSECOND=\"value with spaces\"\n");

    const auto result = sparenode::configuration::EnvironmentFile::load(environment_file);

    REQUIRE(result);
    REQUIRE(result->size() == 2);
    CHECK(result->find("FIRST") == std::optional<std::string_view>{"value"});
    CHECK(result->find("SECOND") == std::optional<std::string_view>{"value with spaces"});
    CHECK_FALSE(result->find("MISSING").has_value());
}

TEST_CASE("Environment file accepts Windows line endings", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "FIRST=value\r\nSECOND=another value\r\n");

    const auto result = sparenode::configuration::EnvironmentFile::load(environment_file);

    REQUIRE(result);
    REQUIRE(result->size() == 2);
    CHECK(result->find("FIRST") == std::optional<std::string_view>{"value"});
    CHECK(result->find("SECOND") == std::optional<std::string_view>{"another value"});
}

TEST_CASE("Environment file reports a missing source", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");

    const auto result = sparenode::configuration::EnvironmentFile::load(directory.path() / ".env");

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::EnvironmentFileErrorCode::file_not_found);
}

TEST_CASE("Environment file rejects malformed assignments", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");
    const auto environment_file = sparenode::test::write_environment_file(directory, "MALFORMED\n");

    const auto result = sparenode::configuration::EnvironmentFile::load(environment_file);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::EnvironmentFileErrorCode::malformed_entry);
    REQUIRE(result.error().line_number == 1);
}

TEST_CASE("Environment file rejects unmatched quotes", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");
    const auto environment_file =
        sparenode::test::write_environment_file(directory, "VARIABLE=\"unfinished\n");

    const auto result = sparenode::configuration::EnvironmentFile::load(environment_file);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::EnvironmentFileErrorCode::malformed_entry);
    REQUIRE(result.error().variable == "VARIABLE");
}

TEST_CASE("Environment file rejects duplicate variables generically", "[configuration][env]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-environment");
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "FUTURE_SETTING=first\nFUTURE_SETTING=second\n");

    const auto result = sparenode::configuration::EnvironmentFile::load(environment_file);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::EnvironmentFileErrorCode::duplicate_variable);
    REQUIRE(result.error().line_number == 2);
    REQUIRE(result.error().variable == "FUTURE_SETTING");
}
