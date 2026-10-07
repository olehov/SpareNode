#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Creates one validated root for directory-creation tests.
[[nodiscard]] sparenode::configuration::SharedRoot
make_root(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    return std::move(root).value();
}

} // namespace

TEST_CASE("Directory creation creates exactly one confined child", "[filesystem][directory-create]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-directory-create");
    const auto parent = directory.path() / "parent";
    REQUIRE(std::filesystem::create_directory(parent));
    const auto root = make_root(directory);

    const auto created = sparenode::filesystem::create_directory(root, "parent/new%20directory");

    REQUIRE(created);
    CHECK(std::filesystem::equivalent(created.value(), parent / "new directory"));
    CHECK(std::filesystem::is_directory(created.value()));
}

TEST_CASE("Directory creation preserves every existing destination",
          "[filesystem][directory-create][conflict]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-directory-conflict");
    REQUIRE(std::filesystem::create_directory(directory.path() / "existing-directory"));
    REQUIRE(std::ofstream(directory.path() / "existing-file").good());
    const auto root = make_root(directory);

    for (const auto *name : {"existing-directory", "existing-file"})
    {
        const auto result = sparenode::filesystem::create_directory(root, name);
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              sparenode::filesystem::DirectoryCreationErrorCode::destination_exists);
    }
    CHECK(std::filesystem::is_directory(directory.path() / "existing-directory"));
    CHECK(std::filesystem::is_regular_file(directory.path() / "existing-file"));
}

TEST_CASE("Directory creation rejects roots missing parents and unsafe paths",
          "[filesystem][directory-create][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-directory-paths");
    const auto root = make_root(directory);

    const auto root_request = sparenode::filesystem::create_directory(root, {});
    REQUIRE_FALSE(root_request);
    CHECK(root_request.error().code ==
          sparenode::filesystem::DirectoryCreationErrorCode::invalid_destination);

    const auto missing_parent = sparenode::filesystem::create_directory(root, "missing/child");
    REQUIRE_FALSE(missing_parent);
    CHECK(missing_parent.error().code ==
          sparenode::filesystem::DirectoryCreationErrorCode::parent_unavailable);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));

    const auto traversal = sparenode::filesystem::create_directory(root, "../escaped");
    REQUIRE_FALSE(traversal);
    CHECK(traversal.error().code ==
          sparenode::filesystem::DirectoryCreationErrorCode::invalid_destination);

    const auto embedded_null = sparenode::filesystem::create_directory(root, "bad%00name");
    REQUIRE_FALSE(embedded_null);
    CHECK(embedded_null.error().code ==
          sparenode::filesystem::DirectoryCreationErrorCode::invalid_destination);
}
