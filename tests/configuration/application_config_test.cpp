#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include "sparenode/configuration/application_config.hpp"
#include "sparenode/configuration/environment_file.hpp"
#include "sparenode/logging/log_severity.hpp"
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
    CHECK_FALSE(result->multithreading_enabled());
    CHECK(result->minimum_log_severity() == sparenode::logging::LogSeverity::info);
}

TEST_CASE("Application configuration reads the minimum log severity",
          "[configuration][application][logging]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto environment_file = sparenode::test::write_environment_file(
        directory,
        "SPARENODE_SHARED_ROOT=" + shared_directory.string() + "\nSPARENODE_LOG_LEVEL=debug\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE(result);
    CHECK(result->minimum_log_severity() == sparenode::logging::LogSeverity::debug);
}

TEST_CASE("Application configuration rejects an invalid minimum log severity",
          "[configuration][application][logging]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto environment_file = sparenode::test::write_environment_file(
        directory,
        "SPARENODE_SHARED_ROOT=" + shared_directory.string() + "\nSPARENODE_LOG_LEVEL=verbose\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          sparenode::configuration::ApplicationConfigErrorCode::invalid_log_severity);
    CHECK(result.error().variable == "SPARENODE_LOG_LEVEL");
}

TEST_CASE("Application configuration reads the multithreading switch",
          "[configuration][application][concurrency]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto environment_file = sparenode::test::write_environment_file(
        directory,
        "SPARENODE_SHARED_ROOT=" + shared_directory.string() + "\nSPARENODE_MULTITHREADING=true\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE(result);
    CHECK(result->multithreading_enabled());
}

TEST_CASE("Application configuration accepts an explicitly disabled multithreading switch",
          "[configuration][application][concurrency]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "SPARENODE_SHARED_ROOT=" + shared_directory.string() +
                       "\nSPARENODE_MULTITHREADING=false\n");
    const auto environment_result =
        sparenode::configuration::EnvironmentFile::load(environment_file);
    REQUIRE(environment_result);

    const auto result =
        sparenode::configuration::ApplicationConfig::create(environment_result.value());

    REQUIRE(result);
    CHECK_FALSE(result->multithreading_enabled());
}

TEST_CASE("Application configuration rejects an invalid multithreading switch",
          "[configuration][application][concurrency]")
{
    for (const std::string_view invalid_value : {std::string_view{}, std::string_view{"auto"}})
    {
        const sparenode::test::TemporaryDirectory directory("sparenode-config");
        const auto shared_directory = directory.path() / "shared";
        std::filesystem::create_directory(shared_directory);
        const auto environment_file = sparenode::test::write_environment_file(
            directory, "SPARENODE_SHARED_ROOT=" + shared_directory.string() +
                           "\nSPARENODE_MULTITHREADING=" + std::string(invalid_value) + '\n');
        const auto environment_result =
            sparenode::configuration::EnvironmentFile::load(environment_file);
        REQUIRE(environment_result);

        const auto result =
            sparenode::configuration::ApplicationConfig::create(environment_result.value());

        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              sparenode::configuration::ApplicationConfigErrorCode::invalid_boolean);
        CHECK(result.error().variable == "SPARENODE_MULTITHREADING");
    }
}

TEST_CASE("Application configuration decodes a UTF-8 shared-root path",
          "[configuration][application]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config");
    const auto shared_directory = directory.path() / std::filesystem::path(u8"спільна папка");
    std::filesystem::create_directory(shared_directory);
    const auto shared_directory_utf8 = shared_directory.u8string();
    const auto environment_file = sparenode::test::write_environment_file(
        directory, "SPARENODE_SHARED_ROOT=\"" +
                       std::string(shared_directory_utf8.begin(), shared_directory_utf8.end()) +
                       "\"\n");
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
