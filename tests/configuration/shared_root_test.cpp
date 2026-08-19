#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "sparenode/configuration/shared_root.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Shared root accepts an existing directory", "[configuration][filesystem]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-shared-root");

    const auto result = sparenode::configuration::SharedRoot::create(directory.path());

    REQUIRE(result);
    REQUIRE(result->path().is_absolute());
    REQUIRE(result->path() == std::filesystem::canonical(directory.path()));
}

TEST_CASE("Shared root canonicalizes redundant path components", "[configuration][filesystem]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-shared-root");
    const auto redundant_path = directory.path() / ".";

    const auto result = sparenode::configuration::SharedRoot::create(redundant_path);

    REQUIRE(result);
    REQUIRE(result->path() == std::filesystem::canonical(directory.path()));
}

TEST_CASE("Shared root rejects an empty path", "[configuration][filesystem]")
{
    const auto result = sparenode::configuration::SharedRoot::create({});

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::configuration::SharedRootErrorCode::empty_path);
}

TEST_CASE("Shared root rejects a missing path", "[configuration][filesystem]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-shared-root");
    const auto missing_path = directory.path() / "missing";

    const auto result = sparenode::configuration::SharedRoot::create(missing_path);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::configuration::SharedRootErrorCode::not_found);
}

TEST_CASE("Shared root rejects a regular file", "[configuration][filesystem]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-shared-root");
    const auto file_path = directory.path() / "file.txt";
    std::ofstream(file_path) << "not a directory";

    const auto result = sparenode::configuration::SharedRoot::create(file_path);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::configuration::SharedRootErrorCode::not_directory);
}
