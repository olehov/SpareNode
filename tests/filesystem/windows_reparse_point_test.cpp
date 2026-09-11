#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "support/temporary_directory.hpp"
#include "support/windows_junction.hpp"

namespace
{

/// @brief Owns a Windows directory tree containing an isolated shared root.
struct JunctionFixture
{
    sparenode::test::TemporaryDirectory directory{"sparenode-junction"}; ///< Test-owned tree.
    std::filesystem::path shared{directory.path() / "shared"};           ///< Shared boundary.
    std::filesystem::path outside{directory.path() / "outside"};         ///< External sibling.

    /// @brief Creates normal shared and external directories.
    JunctionFixture()
    {
        std::filesystem::create_directory(shared);
        std::filesystem::create_directory(outside);
    }

    /// @brief Creates a junction and requires native success.
    /// @param[in] target Existing directory targeted by the junction.
    /// @param[in] link Path at which to create the junction.
    void create_junction(const std::filesystem::path &target,
                         const std::filesystem::path &link) const
    {
        const auto error = sparenode::test::create_directory_junction(target, link);
        INFO(error.message());
        REQUIRE_FALSE(error);
    }

    /// @brief Resolves an untrusted request against the shared fixture root.
    /// @param[in] request Relative request path.
    /// @return Safe-path result for the request.
    [[nodiscard]] auto resolve(const std::string_view request) const
    {
        auto root = sparenode::configuration::SharedRoot::create(shared);
        REQUIRE(root);
        return sparenode::filesystem::SafePath::resolve(root.value(), request);
    }
};

} // namespace

TEST_CASE("Safe path permits an internal Windows directory junction",
          "[filesystem][safe-path][windows][junction][security]")
{
    const JunctionFixture fixture;
    const auto target = fixture.shared / "target";
    std::filesystem::create_directory(target);
    REQUIRE(std::ofstream(target / "file.txt").good());
    fixture.create_junction(target, fixture.shared / "link");
    fixture.create_junction(target.parent_path() / "TARGET", fixture.shared / "case-link");

    const auto directory = fixture.resolve("link");
    REQUIRE(directory);
    REQUIRE(std::filesystem::equivalent(directory->path(), target));
    const auto file = fixture.resolve("link/file.txt");
    REQUIRE(file);
    REQUIRE(std::filesystem::equivalent(file->path(), target / "file.txt"));
    const auto missing = fixture.resolve("link/new/file.txt");
    REQUIRE(missing);
    REQUIRE(missing->path() == directory->path() / "new/file.txt");
    const auto differently_cased = fixture.resolve("case-link/file.txt");
    REQUIRE(differently_cased);
    REQUIRE(std::filesystem::equivalent(differently_cased->path(), target / "file.txt"));
}

TEST_CASE("Safe path rejects a Windows junction outside the shared root",
          "[filesystem][safe-path][windows][junction][security]")
{
    const JunctionFixture fixture;
    fixture.create_junction(fixture.outside, fixture.shared / "link");

    for (const auto request : {"link", "link/file.txt", "link/missing/file.txt"})
    {
        CAPTURE(request);
        const auto result = fixture.resolve(request);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code ==
                sparenode::filesystem::SafePathErrorCode::outside_shared_root);
    }
}

TEST_CASE("Safe path rejects nested Windows junction redirection outside the root",
          "[filesystem][safe-path][windows][junction][security]")
{
    const JunctionFixture fixture;
    const auto middle = fixture.shared / "middle";
    std::filesystem::create_directory(middle);
    fixture.create_junction(fixture.outside, middle / "external");
    fixture.create_junction(middle, fixture.shared / "first");

    const auto result = fixture.resolve("first/external/secret.txt");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path rejects an unsupported reparse target behind a Windows junction",
          "[filesystem][safe-path][windows][junction][reparse][security]")
{
    const JunctionFixture fixture;
    const auto unsupported = fixture.shared / "unsupported";
    const auto error = sparenode::test::create_unsupported_directory_reparse_point(unsupported);
    INFO(error.message());
    REQUIRE_FALSE(error);
    fixture.create_junction(unsupported, fixture.shared / "link");

    const auto result = fixture.resolve("link");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            sparenode::filesystem::SafePathErrorCode::unsupported_reparse_point);
    REQUIRE(std::filesystem::remove(fixture.shared / "link"));
    REQUIRE(std::filesystem::remove(unsupported));
}

TEST_CASE("Safe path rejects a shared root replaced by a Windows junction",
          "[filesystem][safe-path][windows][junction][security]")
{
    const JunctionFixture fixture;
    auto root = sparenode::configuration::SharedRoot::create(fixture.shared);
    REQUIRE(root);
    std::filesystem::remove(fixture.shared);
    fixture.create_junction(fixture.outside, fixture.shared);

    const auto result = sparenode::filesystem::SafePath::resolve(root.value(), "secret.txt");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path keeps normal Windows directories unchanged",
          "[filesystem][safe-path][windows][security]")
{
    const JunctionFixture fixture;
    const auto directory = fixture.shared / "ordinary";
    std::filesystem::create_directory(directory);

    const auto parent = fixture.resolve("ordinary");
    REQUIRE(parent);
    const auto result = fixture.resolve("ordinary/new.txt");
    REQUIRE(result);
    REQUIRE(result->path() == parent->path() / "new.txt");
}

TEST_CASE("Safe path rejects a reparse target with a trailing-period component",
          "[filesystem][safe-path][windows][junction][security]")
{
    const JunctionFixture fixture;
    fixture.create_junction(fixture.outside, fixture.shared / "dot");
    REQUIRE(std::ofstream(fixture.outside / "marker.txt") << "outside");

    const auto exact_directory = sparenode::test::extended_windows_path(fixture.shared / "dot.");
    REQUIRE(CreateDirectoryW(exact_directory.c_str(), nullptr) != FALSE);
    REQUIRE(std::ofstream(exact_directory / "marker.txt") << "inside");
    fixture.create_junction(fixture.shared / "dot.", fixture.shared / "alias");

    const auto result = fixture.resolve("alias/marker.txt");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::invalid_component);

    REQUIRE(std::filesystem::remove(fixture.shared / "alias"));
    REQUIRE(std::filesystem::remove(exact_directory / "marker.txt"));
    REQUIRE(std::filesystem::remove(exact_directory));
}

#endif
