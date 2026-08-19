#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "sparenode/configuration/application_config.hpp"
#include "sparenode/configuration/environment_file.hpp"
#include "support/environment_file.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Application configuration reads its shared root from parsed variables",
          "[configuration][application]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / "shared directory";
    std::filesystem::create_directory(shared_directory);
    const auto environment_file = sparenode::test::write_environment_file(
        directory,
        "FUTURE_SETTING=preserved\nSPARENODE_SHARED_ROOT=\"" + shared_directory.string() + "\"\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE(result);
    REQUIRE(result->shared_root().path() == std::filesystem::canonical(shared_directory));
}

TEST_CASE("Application configuration requires the shared-root variable",
          "[configuration][application]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto environment_file =
        sparenode::test::write_environment_file(directory, "FUTURE_SETTING=value\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::ApplicationConfigErrorCode::missing_shared_root);
}

TEST_CASE("Application configuration rejects an empty shared root", "[configuration][application]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "SPARENODE_SHARED_ROOT=\nFUTURE_SETTING=value\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::ApplicationConfigErrorCode::missing_value);
}

TEST_CASE("Application configuration preserves shared-root validation details",
          "[configuration][application]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto missing_path = directory.path() / "missing";
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "SPARENODE_SHARED_ROOT=" + missing_path.string() + '\n');
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::configuration::ApplicationConfigErrorCode::invalid_shared_root);
    const auto shared_root_error =
        result.error().shared_root_error.value_or(sparenode::configuration::SharedRootError{
            sparenode::configuration::SharedRootErrorCode::empty_path, {}});
    REQUIRE(result.error().shared_root_error.has_value());
    REQUIRE(shared_root_error.code == sparenode::configuration::SharedRootErrorCode::not_found);
}
