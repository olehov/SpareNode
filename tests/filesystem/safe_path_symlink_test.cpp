#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "support/temporary_directory.hpp"

namespace
{
/// @brief Owns isolated shared and external directories for confinement tests.
struct SymlinkFixture
{
    sparenode::test::TemporaryDirectory directory{"sparenode-symlink"}; ///< Test-owned tree.
    std::filesystem::path shared{directory.path() / "shared"};          ///< Configured boundary.
    std::filesystem::path outside{directory.path() / "shared-private"}; ///< Similar-name sibling.

    /// @brief Creates both directories before validating the shared root.
    SymlinkFixture()
    {
        std::filesystem::create_directory(shared);
        std::filesystem::create_directory(outside);
    }

    /// @brief Validates the fixture root and resolves a request against it.
    /// @param[in] request Relative URL path to resolve.
    /// @return Safe-path result for inspection by the test.
    [[nodiscard]] auto resolve(const std::string_view request) const
    {
        auto root = sparenode::configuration::SharedRoot::create(shared);
        REQUIRE(root);
        return sparenode::filesystem::SafePath::resolve(root.value(), request);
    }
};

/// @brief Creates a real symbolic link, skipping only Windows privilege limitations.
/// @param[in] target Stored link target, absolute or relative to the link's parent.
/// @param[in] link Location of the new symbolic link.
/// @param[in] directory Whether to create a directory link on Windows.
void create_link(const std::filesystem::path &target, const std::filesystem::path &link,
                 const bool directory = true)
{
    std::error_code error;
    if (directory)
    {
        std::filesystem::create_directory_symlink(target, link, error);
    }
    else
    {
        std::filesystem::create_symlink(target, link, error);
    }
#if defined(_WIN32) && !defined(SPARENODE_REQUIRE_SYMLINK_TESTS)
    constexpr int privilege_not_held = 1314; // Win32 ERROR_PRIVILEGE_NOT_HELD.
    if (error == std::error_code(privilege_not_held, std::system_category()))
    {
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
    }
#endif
    INFO(error.message());
    REQUIRE_FALSE(error);
}
} // namespace

TEST_CASE("Safe path follows internal absolute and relative directory links",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    const auto target = fixture.shared / "target";
    std::filesystem::create_directory(target);
    create_link(target, fixture.shared / "absolute");
    create_link("target", fixture.shared / "relative");

    for (const auto request : {"absolute", "relative", "absolute/new/file.txt"})
    {
        CAPTURE(request);
        const auto result = fixture.resolve(request);
        REQUIRE(result);
        const auto canonical_target = std::filesystem::canonical(target);
        const auto expected = request == std::string_view("absolute/new/file.txt")
                                  ? canonical_target / "new/file.txt"
                                  : canonical_target;
        REQUIRE(result->path() == expected);
    }
}

TEST_CASE("Safe path follows internal file links and rejects external file links",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    const auto inside = fixture.shared / "file.txt";
    const auto outside = fixture.outside / "secret.txt";
    REQUIRE(std::ofstream(inside).good());
    REQUIRE(std::ofstream(outside).good());
    create_link(inside, fixture.shared / "internal", false);
    create_link(outside, fixture.shared / "external", false);

    const auto accepted = fixture.resolve("internal");
    REQUIRE(accepted);
    REQUIRE(accepted->path() == std::filesystem::canonical(inside));
    const auto rejected = fixture.resolve("external");
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path rejects external directory links even with a missing suffix",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    create_link(fixture.outside, fixture.shared / "absolute");
    create_link("../shared-private", fixture.shared / "relative");
    for (const auto request : {"absolute", "relative", "absolute/missing/file.txt",
                               "relative%2Fmissing.txt", "relative\\missing.txt"})
    {
        CAPTURE(request);
        const auto result = fixture.resolve(request);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code ==
                sparenode::filesystem::SafePathErrorCode::outside_shared_root);
        REQUIRE(result.error().requested_path == request);
    }
}

TEST_CASE("Safe path checks the final target of nested symbolic links",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    std::filesystem::create_directory(fixture.shared / "target");
    create_link("target", fixture.shared / "second");
    create_link("second", fixture.shared / "first");
    const auto accepted = fixture.resolve("first");
    REQUIRE(accepted);
    REQUIRE(accepted->path() == std::filesystem::canonical(fixture.shared / "target"));

    std::filesystem::remove(fixture.shared / "second");
    create_link(fixture.outside, fixture.shared / "second");
    const auto rejected = fixture.resolve("first");
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path rejects dangling symbolic links and cycles",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    create_link("missing", fixture.shared / "broken");
    create_link("missing-file", fixture.shared / "broken-file", false);
    create_link(fixture.outside / "missing", fixture.shared / "broken-outside");
    create_link("cycle-b", fixture.shared / "cycle-a");
    create_link("cycle-a", fixture.shared / "cycle-b");
    for (const auto request : {"broken", "broken-file", "broken/file.txt", "broken-outside",
                               "cycle-a", "cycle-a/file.txt"})
    {
        CAPTURE(request);
        const auto result = fixture.resolve(request);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::resolution_failed);
    }
}

TEST_CASE("Safe path resolves only the lexically normalized request",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    create_link(fixture.outside, fixture.shared / "link");
    const auto result = fixture.resolve("link/../file.txt");
    REQUIRE(result);
    REQUIRE(result->path() == std::filesystem::canonical(fixture.shared) / "file.txt");
}

TEST_CASE("Safe path rejects a shared root replaced with an external link",
          "[filesystem][safe-path][symlink][security]")
{
    const SymlinkFixture fixture;
    auto root = sparenode::configuration::SharedRoot::create(fixture.shared);
    REQUIRE(root);
    std::filesystem::remove(fixture.shared);
    create_link(fixture.outside, fixture.shared);
    const auto result = sparenode::filesystem::SafePath::resolve(root.value(), "missing.txt");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path fails closed when an existing prefix is not a directory",
          "[filesystem][safe-path][security]")
{
    const SymlinkFixture fixture;
    REQUIRE(std::ofstream(fixture.shared / "file.txt").good());
    const auto result = fixture.resolve("file.txt/child");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::resolution_failed);
}
