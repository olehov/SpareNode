#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "sparenode/filesystem/temporary_file.hpp"

namespace
{
/// @brief Borrows string fixture storage as immutable bytes.
[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

/// @brief Reads a completed temporary file without changing its ownership.
[[nodiscard]] std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("Temporary file writes complete content and removes the owned path",
          "[filesystem][temporary-file]")
{
    std::filesystem::path path;
    {
        auto created = sparenode::filesystem::TemporaryFile::create();
        REQUIRE(created.has_value());
        auto file = std::move(created).value();
        path = file.path();
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(file.write(bytes("first")).has_value());
        REQUIRE(file.write(bytes("-second")).has_value());
        CHECK(file.size() == 12);
        REQUIRE(file.complete().has_value());
        CHECK(file.completed());
        CHECK(read_file(path) == "first-second");
        const auto late_write = file.write(bytes("ignored"));
        REQUIRE_FALSE(late_write.has_value());
        CHECK(late_write.error().code ==
              sparenode::filesystem::TemporaryFileErrorCode::invalid_state);
    }
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Temporary file removes an incomplete artifact", "[filesystem][temporary-file]")
{
    std::filesystem::path path;
    {
        auto created = sparenode::filesystem::TemporaryFile::create();
        REQUIRE(created.has_value());
        auto file = std::move(created).value();
        path = file.path();
        REQUIRE(file.write(bytes("partial")).has_value());
        REQUIRE(std::filesystem::exists(path));
    }
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Temporary file release transfers path cleanup responsibility",
          "[filesystem][temporary-file]")
{
    auto created = sparenode::filesystem::TemporaryFile::create();
    REQUIRE(created.has_value());
    auto file = std::move(created).value();
    const auto early_release = file.release();
    REQUIRE_FALSE(early_release.has_value());
    CHECK(early_release.error().code ==
          sparenode::filesystem::TemporaryFileErrorCode::invalid_state);
    REQUIRE(file.write(bytes("published")).has_value());
    REQUIRE(file.complete().has_value());
    auto released = file.release();
    REQUIRE(released.has_value());
    const auto path = std::move(released).value();
    REQUIRE(std::filesystem::exists(path));
    CHECK(read_file(path) == "published");
    std::error_code error;
    CHECK(std::filesystem::remove(path, error));
    CHECK_FALSE(error);
}
