#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Creates a validated shared root for a test-owned directory.
/// @param[in] path Existing directory to validate.
/// @return Validated shared root.
[[nodiscard]] sparenode::configuration::SharedRoot
require_shared_root(const std::filesystem::path &path)
{
    auto result = sparenode::configuration::SharedRoot::create(path);
    REQUIRE(result);
    return std::move(result).value();
}

/// @brief Copies UTF-8 code units into the byte-oriented public request format.
/// @param[in] value UTF-8 code units to copy.
/// @return Byte string containing the same UTF-8 representation.
[[nodiscard]] std::string as_utf8_bytes(const std::u8string_view value)
{
    return {value.begin(), value.end()};
}

} // namespace

TEST_CASE("Safe path resolves a UTF-8 descendant", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_directory = directory.path() / "shared";
    const auto child = shared_directory / std::filesystem::path(u8"дані");
    std::filesystem::create_directories(child);
    const auto shared_root = require_shared_root(shared_directory);

    const auto result =
        sparenode::filesystem::SafePath::resolve(shared_root, as_utf8_bytes(u8"дані/file.txt"));

    REQUIRE(result);
    REQUIRE(result->path() ==
            (shared_root.path() / std::filesystem::path(u8"дані") / "file.txt").lexically_normal());
}

TEST_CASE("Safe path resolves an empty request to the shared root", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "");
    const auto current_directory_result =
        sparenode::filesystem::SafePath::resolve(shared_root, ".");

    REQUIRE(result);
    REQUIRE(result->path() == shared_root.path());
    REQUIRE(current_directory_result);
    REQUIRE(current_directory_result->path() == shared_root.path());
}

TEST_CASE("Safe path permits a missing descendant inside the shared root",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "new/file.txt");

    REQUIRE(result);
    REQUIRE(result->path() == (shared_root.path() / "new/file.txt").lexically_normal());
}

TEST_CASE("Safe path normalizes components that remain inside the shared root",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "folder/../file.txt");

    REQUIRE(result);
    REQUIRE(result->path() == (shared_root.path() / "file.txt").lexically_normal());
}

TEST_CASE("Safe path accepts a request at the application length limit", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::string requested_path(sparenode::filesystem::SafePath::maximum_requested_path_bytes,
                                     'a');

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

    REQUIRE(result);
}

TEST_CASE("Safe path rejects a request above the application length limit",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::string requested_path(
        sparenode::filesystem::SafePath::maximum_requested_path_bytes + 1, 'a');

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::path_too_long);
    REQUIRE(result.error().requested_path.empty());
}

TEST_CASE("Safe path rejects parent traversal outside the shared root", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto shared_root = require_shared_root(shared_directory);

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "../outside.txt");

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path uses components instead of string-prefix containment",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_directory = directory.path() / "shared";
    std::filesystem::create_directory(shared_directory);
    const auto shared_root = require_shared_root(shared_directory);

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "../shared-private");

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::outside_shared_root);
}

TEST_CASE("Safe path rejects rooted input", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const auto root_path_utf8 = std::filesystem::temp_directory_path().root_path().u8string();
    const std::string rooted_path(root_path_utf8.begin(), root_path_utf8.end());

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, rooted_path);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::rooted_path);
}

TEST_CASE("Safe path rejects an embedded null byte", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::string requested_path("file\0hidden", 11);

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::embedded_null);
}

TEST_CASE("Safe path rejects invalid UTF-8", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::array invalid_paths{
        std::string("\xC3\x28", 2),         // Invalid continuation byte.
        std::string("\xC0\xAF", 2),         // Overlong representation.
        std::string("\xED\xA0\x80", 3),     // UTF-16 surrogate code point.
        std::string("\xF4\x90\x80\x80", 4), // Code point above U+10FFFF.
        std::string("\xE2\x82", 2),         // Truncated multibyte sequence.
    };

    for (const auto &requested_path : invalid_paths)
    {
        const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

        CAPTURE(requested_path);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::invalid_encoding);
        REQUIRE(result.error().requested_path == requested_path);
    }
}

#ifdef _WIN32
TEST_CASE("Safe path rejects a Windows drive-relative path", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());

    const auto result = sparenode::filesystem::SafePath::resolve(shared_root, "C:relative.txt");

    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == sparenode::filesystem::SafePathErrorCode::rooted_path);
}

TEST_CASE("Safe path rejects Windows trailing-space and trailing-period aliases",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::array requested_paths{
        std::string(".. /outside.txt"),
        std::string("directory./file.txt"),
    };

    for (const auto &requested_path : requested_paths)
    {
        const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

        CAPTURE(requested_path);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code ==
                sparenode::filesystem::SafePathErrorCode::ambiguous_component);
        REQUIRE(result.error().requested_path == requested_path);
    }
}

TEST_CASE("Safe path rejects Windows reserved device-name aliases", "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::array requested_paths{
        std::string("NUL"),           std::string("con.txt"),    std::string("PRN.log"),
        std::string("AUX"),           std::string("COM1"),       std::string("LPT9"),
        std::string("folder/NUL"),    std::string("NUL:stream"), as_utf8_bytes(u8"COM\u00B9.data"),
        as_utf8_bytes(u8"lpt\u00B3"),
    };

    for (const auto &requested_path : requested_paths)
    {
        const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

        CAPTURE(requested_path);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code ==
                sparenode::filesystem::SafePathErrorCode::ambiguous_component);
        REQUIRE(result.error().requested_path == requested_path);
    }
}

TEST_CASE("Safe path preserves Windows names that only resemble device aliases",
          "[filesystem][safe-path]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-safe-path");
    const auto shared_root = require_shared_root(directory.path());
    const std::array requested_paths{
        std::string("COM0"),       std::string("COM10"), std::string("LPT0.txt"),
        std::string("serial.txt"), std::string("null"),
    };

    for (const auto &requested_path : requested_paths)
    {
        const auto result = sparenode::filesystem::SafePath::resolve(shared_root, requested_path);

        CAPTURE(requested_path);
        REQUIRE(result);
    }
}
#endif
