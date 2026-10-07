#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "support/temporary_directory.hpp"
#ifdef _WIN32
#include "support/windows_junction.hpp"
#endif

namespace
{

[[nodiscard]] sparenode::configuration::SharedRoot
make_root(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    return std::move(root).value();
}

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

} // namespace

TEST_CASE("Filesystem entry removal deletes files and empty directories", "[filesystem][delete]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-delete-entry");
    REQUIRE(std::ofstream(directory.path() / "file.txt") << "content");
    REQUIRE(std::filesystem::create_directory(directory.path() / "empty"));
    const auto root = make_root(directory);

    REQUIRE(sparenode::filesystem::remove_filesystem_entry(root, "file.txt"));
    REQUIRE(sparenode::filesystem::remove_filesystem_entry(root, "empty"));
    CHECK(!std::filesystem::exists(directory.path() / "file.txt"));
    CHECK(!std::filesystem::exists(directory.path() / "empty"));
}

TEST_CASE("Filesystem entry removal rejects roots missing paths and nonempty directories",
          "[filesystem][delete][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-delete-errors");
    const sparenode::test::TemporaryDirectory outside("sparenode-delete-outside");
    const auto populated = directory.path() / "populated";
    REQUIRE(std::filesystem::create_directory(populated));
    REQUIRE(std::ofstream(populated / "child.txt") << "content");
    REQUIRE(std::ofstream(outside.path() / "retained.txt") << "content");
    const auto root = make_root(directory);

    const auto root_result = sparenode::filesystem::remove_filesystem_entry(root, {});
    REQUIRE(!root_result);
    CHECK(root_result.error().code ==
          sparenode::filesystem::FilesystemEntryRemovalErrorCode::invalid_destination);

    const auto missing = sparenode::filesystem::remove_filesystem_entry(root, "missing");
    REQUIRE(!missing);
    CHECK(missing.error().code ==
          sparenode::filesystem::FilesystemEntryRemovalErrorCode::not_found);

    const auto nonempty = sparenode::filesystem::remove_filesystem_entry(root, "populated");
    REQUIRE(!nonempty);
    CHECK(nonempty.error().code ==
          sparenode::filesystem::FilesystemEntryRemovalErrorCode::directory_not_empty);
    CHECK(std::filesystem::exists(populated / "child.txt"));

    const auto traversal_path = "../" + outside.path().filename().string() + "/retained.txt";
    const auto traversal = sparenode::filesystem::remove_filesystem_entry(root, traversal_path);
    REQUIRE(!traversal);
    CHECK(traversal.error().code ==
          sparenode::filesystem::FilesystemEntryRemovalErrorCode::invalid_destination);
    CHECK(std::filesystem::exists(outside.path() / "retained.txt"));
}

TEST_CASE("Filesystem entry removal deletes an internal link instead of its target",
          "[filesystem][delete][symlink][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-delete-link");
    const auto target = directory.path() / "target";
    REQUIRE(std::filesystem::create_directory(target));
    REQUIRE(std::ofstream(target / "retained.txt") << "content");
    const auto link = directory.path() / "link";
    create_directory_link(target, link);
    const auto root = make_root(directory);

    REQUIRE(sparenode::filesystem::remove_filesystem_entry(root, "link"));
    CHECK(std::filesystem::symlink_status(link).type() == std::filesystem::file_type::not_found);
    CHECK(std::filesystem::exists(target / "retained.txt"));
}

TEST_CASE("Filesystem entry removal rejects links outside the shared root",
          "[filesystem][delete][symlink][security]")
{
    const sparenode::test::TemporaryDirectory shared("sparenode-delete-link-shared");
    const sparenode::test::TemporaryDirectory outside("sparenode-delete-link-outside");
    REQUIRE(std::ofstream(outside.path() / "retained.txt") << "content");
    const auto link = shared.path() / "external";
    create_directory_link(outside.path(), link);
    const auto root = make_root(shared);

    const auto result = sparenode::filesystem::remove_filesystem_entry(root, "external");
    REQUIRE(!result);
    CHECK(result.error().code ==
          sparenode::filesystem::FilesystemEntryRemovalErrorCode::invalid_destination);
    CHECK(std::filesystem::exists(link));
    CHECK(std::filesystem::exists(outside.path() / "retained.txt"));
}

#ifdef _WIN32

TEST_CASE("Filesystem entry removal deletes an internal junction instead of its target",
          "[filesystem][delete][windows][junction][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-delete-junction");
    const auto target = directory.path() / "target";
    REQUIRE(std::filesystem::create_directory(target));
    REQUIRE(std::ofstream(target / "retained.txt") << "content");
    const auto junction = directory.path() / "junction";
    const auto error = sparenode::test::create_directory_junction(target, junction);
    INFO(error.message());
    REQUIRE(error.value() == 0);
    const auto root = make_root(directory);

    REQUIRE(sparenode::filesystem::remove_filesystem_entry(root, "junction"));
    CHECK(std::filesystem::symlink_status(junction).type() ==
          std::filesystem::file_type::not_found);
    CHECK(std::filesystem::exists(target / "retained.txt"));
}

#endif
