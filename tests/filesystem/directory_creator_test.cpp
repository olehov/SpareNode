#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "support/temporary_directory.hpp"
#ifdef _WIN32
#include "support/windows_junction.hpp"
#endif

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

/// @brief Creates a directory symbolic link, skipping unavailable Windows privileges.
void create_directory_link(const std::filesystem::path &target, const std::filesystem::path &link)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
#if defined(_WIN32) && !defined(SPARENODE_REQUIRE_SYMLINK_TESTS)
    constexpr int privilege_not_held = 1314; // Win32 ERROR_PRIVILEGE_NOT_HELD.
    if (error == std::error_code(privilege_not_held, std::system_category()))
    {
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
    }
#endif
    INFO(error.message());
    REQUIRE(error.value() == 0);
}

/// @brief Verifies that a rejected destination retains its safe-path confinement error.
void require_outside_root(const sparenode::filesystem::DirectoryCreationError &error)
{
    CHECK(error.code == sparenode::filesystem::DirectoryCreationErrorCode::invalid_destination);
    const auto path_error = error.path_error.value_or(sparenode::filesystem::SafePathError{});
    REQUIRE(error.path_error.has_value());
    CHECK(path_error.code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
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

TEST_CASE("Directory creation follows internal links and rejects external link parents",
          "[filesystem][directory-create][symlink][security]")
{
    const sparenode::test::TemporaryDirectory shared("sparenode-directory-link-shared");
    const sparenode::test::TemporaryDirectory outside("sparenode-directory-link-outside");
    const auto inside = shared.path() / "inside";
    REQUIRE(std::filesystem::create_directory(inside));
    create_directory_link(inside, shared.path() / "internal");
    create_directory_link(outside.path(), shared.path() / "external");
    const auto root = make_root(shared);

    const auto created = sparenode::filesystem::create_directory(root, "internal/new");
    REQUIRE(created);
    CHECK(std::filesystem::is_directory(inside / "new"));

    const auto rejected = sparenode::filesystem::create_directory(root, "external/new");
    REQUIRE(!rejected);
    require_outside_root(rejected.error());
    CHECK(!std::filesystem::exists(outside.path() / "new"));
}

#ifdef _WIN32

TEST_CASE("Directory creation follows internal junctions and rejects external junction parents",
          "[filesystem][directory-create][windows][junction][security]")
{
    const sparenode::test::TemporaryDirectory shared("sparenode-directory-junction-shared");
    const sparenode::test::TemporaryDirectory outside("sparenode-directory-junction-outside");
    const auto inside = shared.path() / "inside";
    REQUIRE(std::filesystem::create_directory(inside));
    const auto internal_error =
        sparenode::test::create_directory_junction(inside, shared.path() / "internal");
    INFO(internal_error.message());
    REQUIRE(internal_error.value() == 0);
    const auto external_error =
        sparenode::test::create_directory_junction(outside.path(), shared.path() / "external");
    INFO(external_error.message());
    REQUIRE(external_error.value() == 0);
    const auto root = make_root(shared);

    const auto created = sparenode::filesystem::create_directory(root, "internal/new");
    REQUIRE(created);
    CHECK(std::filesystem::is_directory(inside / "new"));

    const auto rejected = sparenode::filesystem::create_directory(root, "external/new");
    REQUIRE(!rejected);
    require_outside_root(rejected.error());
    CHECK(!std::filesystem::exists(outside.path() / "new"));
}

#endif
