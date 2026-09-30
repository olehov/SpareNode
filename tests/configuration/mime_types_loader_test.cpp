#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "sparenode/configuration/mime_types_loader.hpp"
#include "sparenode/http/mime_type_registry.hpp"
#include "support/temporary_directory.hpp"

namespace
{

[[nodiscard]] std::filesystem::path
write_mime_types(const sparenode::test::TemporaryDirectory &directory,
                 const std::string_view source)
{
    const auto path = directory.path() / "mime.types";
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    REQUIRE(output.good());
    return path;
}

} // namespace

TEST_CASE("MIME types loader accepts comments CRLF and multiple extensions",
          "[configuration][mime-types]")
{
    const auto result =
        sparenode::configuration::MimeTypesLoader::parse("# Browser-safe mappings\r\n"
                                                         "image/jpeg JPEG jpg # both spellings\r\n"
                                                         "text/plain txt log\r\n");

    REQUIRE(result.has_value());
    CHECK(result->size() == 4);
    CHECK(result->content_type_for_path("folder/photo.JpG") == "image/jpeg");
    CHECK(result->content_type_for_path("notes.LOG") == "text/plain");
    CHECK(result->content_type_for_path("unknown.bin") == "application/octet-stream");
    CHECK(result->content_type_for_path("extensionless") == "application/octet-stream");
}

TEST_CASE("MIME registry forces browser-executable content to plain text",
          "[configuration][mime-types][security]")
{
    auto registry = sparenode::http::MimeTypeRegistry::create({{"html", "image/png"},
                                                               {"custom", "text/html"},
                                                               {"legacy", "text/ecmascript"},
                                                               {"feed", "application/atom+xml"},
                                                               {"svg", "image/svg+xml"}});

    REQUIRE(registry.has_value());
    CHECK(registry->content_type_for_path("page.html") == "text/plain; charset=utf-8");
    CHECK(registry->content_type_for_path("page.custom") == "text/plain; charset=utf-8");
    CHECK(registry->content_type_for_path("script.legacy") == "text/plain; charset=utf-8");
    CHECK(registry->content_type_for_path("news.feed") == "text/plain; charset=utf-8");
    CHECK(registry->content_type_for_path("icon.svg") == "text/plain; charset=utf-8");
}

TEST_CASE("MIME types loader rejects malformed ambiguous and oversized input",
          "[configuration][mime-types]")
{
    using Code = sparenode::configuration::MimeTypesLoadErrorCode;

    SECTION("missing extension")
    {
        const auto result = sparenode::configuration::MimeTypesLoader::parse("text/plain\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::invalid_line);
        CHECK(result.error().line == 1);
    }

    SECTION("invalid media type")
    {
        const auto result = sparenode::configuration::MimeTypesLoader::parse("text txt\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::invalid_media_type);
    }

    SECTION("invalid extension")
    {
        const auto result =
            sparenode::configuration::MimeTypesLoader::parse("text/plain bad.ext\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::invalid_extension);
    }

    SECTION("extension length bound")
    {
        const std::string accepted_extension(
            sparenode::http::MimeTypeRegistry::maximum_extension_bytes, 'a');
        const auto accepted = sparenode::configuration::MimeTypesLoader::parse(
            "text/plain " + accepted_extension + "\n");
        REQUIRE(accepted.has_value());

        const auto rejected = sparenode::configuration::MimeTypesLoader::parse(
            "text/plain " + accepted_extension + "a\n");
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == Code::invalid_extension);
    }

    SECTION("media type length bound")
    {
        const std::string accepted_media_type =
            "a/" +
            std::string(sparenode::http::MimeTypeRegistry::maximum_media_type_bytes - 2, 'b');
        const auto accepted =
            sparenode::configuration::MimeTypesLoader::parse(accepted_media_type + " ext\n");
        REQUIRE(accepted.has_value());

        const auto rejected =
            sparenode::configuration::MimeTypesLoader::parse(accepted_media_type + "b ext\n");
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == Code::invalid_media_type);
    }

    SECTION("case-insensitive duplicate")
    {
        const auto result = sparenode::configuration::MimeTypesLoader::parse(
            "text/plain txt\napplication/octet-stream TXT\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::duplicate_extension);
        CHECK(result.error().line == 2);
    }

    SECTION("line length bound")
    {
        const std::string source(sparenode::configuration::MimeTypesLoader::maximum_line_bytes + 1,
                                 'a');
        const auto result = sparenode::configuration::MimeTypesLoader::parse(source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::line_too_long);
    }

    SECTION("entry count bound")
    {
        std::string source;
        for (std::size_t index = 0; index < sparenode::http::MimeTypeRegistry::maximum_entries + 1;
             ++index)
        {
            source += "text/plain x" + std::to_string(index) + "\n";
        }
        const auto result = sparenode::configuration::MimeTypesLoader::parse(source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::too_many_entries);
    }
}

TEST_CASE("MIME types loader bounds file input and reports file failures",
          "[configuration][mime-types]")
{
    using Code = sparenode::configuration::MimeTypesLoadErrorCode;
    const sparenode::test::TemporaryDirectory directory("sparenode-mime-types");

    const auto missing =
        sparenode::configuration::MimeTypesLoader::load(directory.path() / "missing.types");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::open_failed);

    const std::string oversized(sparenode::configuration::MimeTypesLoader::maximum_file_bytes + 1,
                                '#');
    const auto oversized_result =
        sparenode::configuration::MimeTypesLoader::load(write_mime_types(directory, oversized));
    REQUIRE_FALSE(oversized_result.has_value());
    CHECK(oversized_result.error().code == Code::file_too_large);
}
