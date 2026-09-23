#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Creates a validated root for one isolated test directory.
[[nodiscard]] sparenode::configuration::SharedRoot
make_root(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    return std::move(root).value();
}

/// @brief Finds one named entry in a successful listing.
[[nodiscard]] const sparenode::filesystem::DirectoryListingEntry &
find_entry(const std::vector<sparenode::filesystem::DirectoryListingEntry> &entries,
           const std::string_view name)
{
    const auto iterator =
        std::ranges::find(entries, name, &sparenode::filesystem::DirectoryListingEntry::name);
    REQUIRE(iterator != entries.end());
    return *iterator;
}

/// @brief Creates a real symbolic link, skipping only Windows privilege limitations.
void create_link(const std::filesystem::path &target, const std::filesystem::path &link,
                 const bool directory)
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

/// @brief Converts system time to the implementation-defined filesystem clock for fixtures.
[[nodiscard]] std::filesystem::file_time_type
to_file_time(const std::chrono::system_clock::time_point value) noexcept
{
    const auto system_now = std::chrono::system_clock::now();
    const auto file_now = std::filesystem::file_time_type::clock::now();
    return file_now + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                          value - system_now);
}

} // namespace

TEST_CASE("Directory listing returns sorted portable metadata", "[filesystem][listing]")
{
    using sparenode::filesystem::DirectoryEntryType;
    const sparenode::test::TemporaryDirectory directory("sparenode-listing");
    std::filesystem::create_directory(directory.path() / "folder");
    {
        std::ofstream file(directory.path() / "alpha.txt", std::ios::binary);
        file << "hello";
    }
    constexpr auto expected_time =
        std::chrono::sys_days(std::chrono::year{2024} / std::chrono::January / 2) +
        std::chrono::hours{3} + std::chrono::minutes{4} + std::chrono::seconds{5};
    std::filesystem::last_write_time(directory.path() / "alpha.txt", to_file_time(expected_time));

    const auto listing = sparenode::filesystem::list_directory(make_root(directory), {});
    REQUIRE(listing);
    REQUIRE(listing->size() == 2);
    CHECK(listing->at(0).name == "alpha.txt");
    CHECK(listing->at(1).name == "folder");
    const auto &file = find_entry(listing.value(), "alpha.txt");
    CHECK(file.type == DirectoryEntryType::regular_file);
    CHECK(file.size == 5);
    const auto timestamp_difference = file.modification_time - expected_time;
    CHECK(timestamp_difference >= std::chrono::seconds{-1});
    CHECK(timestamp_difference <= std::chrono::seconds{1});
    const auto &folder = find_entry(listing.value(), "folder");
    CHECK(folder.type == DirectoryEntryType::directory);
    CHECK_FALSE(folder.size);
}

TEST_CASE("Directory listing resolves an encoded nested path", "[filesystem][listing][path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-listing-nested");
    const auto nested = directory.path() / "nested folder";
    std::filesystem::create_directory(nested);
    REQUIRE(std::ofstream(nested / "inside.txt").good());

    const auto listing =
        sparenode::filesystem::list_directory(make_root(directory), "nested%20folder");
    REQUIRE(listing);
    REQUIRE(listing->size() == 1);
    CHECK(listing->front().name == "inside.txt");
}

TEST_CASE("Directory listing reports safe request failures", "[filesystem][listing][errors]")
{
    using sparenode::filesystem::DirectoryListingErrorCode;
    const sparenode::test::TemporaryDirectory directory("sparenode-listing-errors");
    REQUIRE(std::ofstream(directory.path() / "file.txt").good());
    const auto root = make_root(directory);

    const auto missing = sparenode::filesystem::list_directory(root, "missing");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == DirectoryListingErrorCode::not_found);

    const auto file = sparenode::filesystem::list_directory(root, "file.txt");
    REQUIRE_FALSE(file);
    CHECK(file.error().code == DirectoryListingErrorCode::not_directory);

    const auto traversal = sparenode::filesystem::list_directory(root, "../outside");
    REQUIRE_FALSE(traversal);
    CHECK(traversal.error().code == DirectoryListingErrorCode::invalid_path);
    REQUIRE(traversal.error().path_error);
}

TEST_CASE("Directory listing hides external links and identifies internal links",
          "[filesystem][listing][symlink][security]")
{
    using sparenode::filesystem::DirectoryEntryType;
    const sparenode::test::TemporaryDirectory fixture("sparenode-listing-links");
    const auto shared = fixture.path() / "shared";
    const auto outside = fixture.path() / "outside";
    std::filesystem::create_directory(shared);
    std::filesystem::create_directory(outside);
    REQUIRE(std::ofstream(shared / "inside.txt").good());
    REQUIRE(std::ofstream(outside / "secret.txt").good());
    create_link(shared / "inside.txt", shared / "internal-link", false);
    create_link(outside / "secret.txt", shared / "external-link", false);
    auto root = sparenode::configuration::SharedRoot::create(shared);
    REQUIRE(root);

    const auto listing = sparenode::filesystem::list_directory(root.value(), {});
    REQUIRE(listing);
    CHECK(std::ranges::none_of(listing.value(),
                               [](const auto &entry) { return entry.name == "external-link"; }));
    const auto &internal = find_entry(listing.value(), "internal-link");
    CHECK(internal.type == DirectoryEntryType::symbolic_link);
    CHECK_FALSE(internal.size);
}

TEST_CASE("Directory listing enforces its entry boundary", "[filesystem][listing][limits]")
{
    using sparenode::filesystem::DirectoryListingErrorCode;
    const sparenode::test::TemporaryDirectory directory("sparenode-listing-limit");
    for (std::size_t index = 0; index <= sparenode::filesystem::maximum_directory_listing_entries;
         ++index)
    {
        REQUIRE(std::ofstream(directory.path() / ("file-" + std::to_string(index))).good());
    }

    const auto listing = sparenode::filesystem::list_directory(make_root(directory), {});
    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == DirectoryListingErrorCode::too_many_entries);
}
