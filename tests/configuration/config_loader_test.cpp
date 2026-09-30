#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/configuration/runtime/location_permissions.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "support/temporary_directory.hpp"
#include "support/windows_junction.hpp"

namespace
{

/// @brief Writes one binary `spnode.conf` fixture inside a temporary directory.
/// @param[in] directory Temporary owner of the fixture path.
/// @param[in] content Exact bytes written to the configuration source.
/// @return Path of the completed fixture.
[[nodiscard]] std::filesystem::path
write_config(const sparenode::test::TemporaryDirectory &directory, const std::string &content)
{
    const auto path = directory.path() / "spnode.conf";
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
    return path;
}

/// @brief Creates valid configuration text exposing the supplied existing directory.
/// @param[in] shared_root Existing directory accepted by SharedRoot.
/// @return Complete explicit version-one configuration.
[[nodiscard]] std::string valid_config(const std::filesystem::path &shared_root)
{
    return "server {\n"
           "bind \"127.0.0.1\";\n"
           "port 8181;\n"
           "multithreading true;\n"
           "worker_threads 3;\n"
           "log_level \"warning\";\n"
           "header_timeout_ms 1200;\n"
           "body_timeout_ms 2400;\n"
           "request_timeout_ms 3600;\n"
           "location \"/api/Documents\" {\n"
           "path \"" +
           shared_root.generic_string() + "\";\nread false;\nwrite true;\ndelete true;\n}\n}";
}

/// @brief Wraps server directives and location source in a complete configuration document.
[[nodiscard]] std::string make_config(const std::string_view server_directives,
                                      const std::string_view locations)
{
    return "server {\n" + std::string(server_directives) + std::string(locations) + "\n}";
}

/// @brief Creates one location block with a caller-selected API path and body.
[[nodiscard]] std::string make_location(const std::string_view api_path,
                                        const std::string_view body)
{
    return "location \"" + std::string(api_path) + "\" {\n" + std::string(body) + "\n}\n";
}

/// @brief Returns whether validation output contains one expected semantic category.
[[nodiscard]] bool
has_validation_error(const sparenode::configuration::ConfigLoadError &error,
                     const sparenode::configuration::ConfigValidationErrorCode expected)
{
    const auto *failures =
        std::get_if<std::vector<sparenode::configuration::ConfigValidationError>>(&error.failure);
    return failures != nullptr && std::ranges::any_of(*failures, [expected](const auto &failure)
                                                      { return failure.code == expected; });
}

} // namespace

TEST_CASE("Configuration loader maps a valid file through every configuration stage",
          "[configuration][loader]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-loader");
    const auto path = write_config(directory, valid_config(directory.path()));

    const auto result = sparenode::configuration::ConfigLoader::load(path);

    REQUIRE(result.has_value());
    REQUIRE(result->servers().size() == 1);
    const auto &server = result->servers().front();
    CHECK(server.endpoint().address == "127.0.0.1");
    CHECK(server.endpoint().port == 8181);
    CHECK(server.effective_worker_count() == 3);
    CHECK(server.minimum_log_severity() == sparenode::logging::LogSeverity::warning);
    CHECK(server.http_timeouts().headers == std::chrono::milliseconds{1200});
    CHECK(server.http_timeouts().body == std::chrono::milliseconds{2400});
    CHECK(server.http_timeouts().total == std::chrono::milliseconds{3600});
    REQUIRE(server.locations().size() == 1);
    CHECK(std::filesystem::equivalent(server.locations().front().root().path(), directory.path()));
    CHECK(server.locations().front().permissions() ==
          sparenode::configuration::runtime::LocationPermissions{false, true, true});
}

TEST_CASE("Configuration loader resolves and loads MIME mappings beside the main file",
          "[configuration][loader][mime-types]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-mime-loader");
    {
        std::ofstream mime_file(directory.path() / "custom.types", std::ios::binary);
        REQUIRE(mime_file.is_open());
        mime_file << "image/png png\ntext/plain txt\n";
        REQUIRE(mime_file.good());
    }
    const auto location =
        make_location("/api/Documents", "path \"" + directory.path().generic_string() + "\";");
    const auto config_path =
        write_config(directory, make_config("mime_types_file \"custom.types\";\n", location));

    const auto result = sparenode::configuration::ConfigLoader::load(config_path);

    REQUIRE(result.has_value());
    REQUIRE(result->servers().size() == 1);
    CHECK(result->servers().front().mime_types().content_type_for_path("image.PNG") == "image/png");
}

TEST_CASE("Configuration loader preserves filesystem resolution in MIME paths",
          "[configuration][loader][mime-types][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-mime-symlink");
    const auto target = directory.path() / "target";
    const auto nested = target / "nested";
    std::filesystem::create_directories(nested);
    const auto link = directory.path() / "link";
    std::error_code link_error;
#ifdef _WIN32
    link_error = sparenode::test::create_directory_junction(nested, link);
#else
    std::filesystem::create_directory_symlink(nested, link, link_error);
#endif
    if (link_error)
    {
        SKIP("Directory redirection creation is unavailable: " << link_error.message());
    }

// Windows normalizes `..` before traversing a reparse point, while POSIX resolves it after
// following the directory symlink. Place the fixture where each platform resolves the raw path.
#ifdef _WIN32
    const auto resolved_parent = directory.path();
#else
    const auto resolved_parent = target;
#endif
    {
        std::ofstream mime_file(resolved_parent / "custom.types", std::ios::binary);
        REQUIRE(mime_file.is_open());
        mime_file << "image/png png\n";
        REQUIRE(mime_file.good());
    }
    REQUIRE(std::filesystem::exists(link / ".." / "custom.types"));
    const auto location =
        make_location("/api/Documents", "path \"" + directory.path().generic_string() + "\";");
    const std::vector<std::pair<std::string, std::string>> configured_paths{
        {"relative", "link/../custom.types"},
        {"absolute", (link / ".." / "custom.types").generic_string()},
    };

    for (const auto &[name, configured_path] : configured_paths)
    {
        DYNAMIC_SECTION(name)
        {
            const auto config_path = write_config(
                directory, make_config("mime_types_file \"" + configured_path + "\";\n", location));
            const auto result = sparenode::configuration::ConfigLoader::load(config_path);

            REQUIRE(result.has_value());
            CHECK(result->servers().front().mime_types().content_type_for_path("image.png") ==
                  "image/png");
        }
    }
}
TEST_CASE("Configuration loader attributes MIME failures to the resolved MIME file",
          "[configuration][loader][mime-types]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-mime-error");
    const auto missing_path = directory.path() / "missing.types";
    const auto location =
        make_location("/api/Documents", "path \"" + directory.path().generic_string() + "\";");
    const auto config_path =
        write_config(directory, make_config("mime_types_file \"missing.types\";\n", location));

    const auto result = sparenode::configuration::ConfigLoader::load(config_path);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().source_path == missing_path);
    REQUIRE(std::holds_alternative<sparenode::configuration::MimeTypesLoadError>(
        result.error().failure));
    CHECK(std::get<sparenode::configuration::MimeTypesLoadError>(result.error().failure).code ==
          sparenode::configuration::MimeTypesLoadErrorCode::open_failed);
    CHECK(sparenode::configuration::format_config_load_error(result.error())
              .find(missing_path.generic_string() +
                    ":1:1: error: unable to open MIME types file") != std::string::npos);
}

TEST_CASE("Configuration loader preserves file lexer parser and validation failures",
          "[configuration][loader]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-loader");

    SECTION("missing file")
    {
        const auto path = directory.path() / "missing.conf";
        const auto result = sparenode::configuration::ConfigLoader::load(path);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(std::holds_alternative<sparenode::configuration::ConfigFileError>(
            result.error().failure));
        CHECK(std::get<sparenode::configuration::ConfigFileError>(result.error().failure).code ==
              sparenode::configuration::ConfigFileErrorCode::open_failed);
    }

    SECTION("oversized source")
    {
        const auto path = write_config(
            directory,
            std::string(sparenode::configuration::ConfigLexer::max_input_size_bytes + 1, ' '));
        const auto result = sparenode::configuration::ConfigLoader::load(path);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(std::holds_alternative<sparenode::configuration::ConfigLexerError>(
            result.error().failure));
        CHECK(std::get<sparenode::configuration::ConfigLexerError>(result.error().failure).code ==
              sparenode::configuration::ConfigLexerErrorCode::input_too_large);
    }

    SECTION("lexical failure")
    {
        const auto path = write_config(directory, "server { @ }");
        const auto result = sparenode::configuration::ConfigLoader::load(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(std::holds_alternative<sparenode::configuration::ConfigLexerError>(
            result.error().failure));
    }

    SECTION("parser failure")
    {
        const auto path = write_config(directory, "server { port 8080 }");
        const auto result = sparenode::configuration::ConfigLoader::load(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(std::holds_alternative<sparenode::configuration::ConfigParserError>(
            result.error().failure));
    }

    SECTION("validation failures")
    {
        const auto path = write_config(directory, "server { port 0; }");
        const auto result = sparenode::configuration::ConfigLoader::load(path);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(
            std::holds_alternative<std::vector<sparenode::configuration::ConfigValidationError>>(
                result.error().failure));
        CHECK(std::get<std::vector<sparenode::configuration::ConfigValidationError>>(
                  result.error().failure)
                  .size() == 2);
    }
}

TEST_CASE("Configuration loader formats source-located diagnostics", "[configuration][loader]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-config-loader");
    const auto path = write_config(directory, "server { port 0; }");
    const auto result = sparenode::configuration::ConfigLoader::load(path);
    REQUIRE_FALSE(result.has_value());

    const auto diagnostic = sparenode::configuration::format_config_load_error(result.error());

    CHECK(diagnostic.find(path.generic_string() + ":1:15: error:") != std::string::npos);
    CHECK(diagnostic.find("port must be between 1 and 65535") != std::string::npos);
    CHECK(diagnostic.find("at least one filesystem location is required") != std::string::npos);
}

TEST_CASE("Configuration loader formats both file I/O failure categories",
          "[configuration][loader][errors]")
{
    using sparenode::configuration::ConfigFileError;
    using sparenode::configuration::ConfigFileErrorCode;
    using sparenode::configuration::ConfigLoadError;
    const std::filesystem::path path = "config/spnode.conf";

    const auto open_diagnostic = sparenode::configuration::format_config_load_error(
        ConfigLoadError{path, ConfigFileError{ConfigFileErrorCode::open_failed}});
    const auto read_diagnostic = sparenode::configuration::format_config_load_error(
        ConfigLoadError{path, ConfigFileError{ConfigFileErrorCode::read_failed}});

    CHECK(open_diagnostic == "error: unable to open configuration file 'config/spnode.conf'");
    CHECK(read_diagnostic == "error: unable to read configuration file 'config/spnode.conf'");
}

TEST_CASE("Configuration loader escapes terminal control bytes in source paths",
          "[configuration][loader][security]")
{
    using sparenode::configuration::ConfigFileError;
    using sparenode::configuration::ConfigFileErrorCode;
    using sparenode::configuration::ConfigLoadError;

    std::string unsafe_path = "config/unsafe";
    unsafe_path.push_back('\x1B');
    unsafe_path.push_back('\x7F');
    unsafe_path += ".conf";
    const auto diagnostic = sparenode::configuration::format_config_load_error(ConfigLoadError{
        std::filesystem::path(unsafe_path), ConfigFileError{ConfigFileErrorCode::open_failed}});
    const auto located_diagnostic = sparenode::configuration::format_config_load_error(
        ConfigLoadError{std::filesystem::path(unsafe_path),
                        sparenode::configuration::ConfigLexerError{
                            sparenode::configuration::ConfigLexerErrorCode::invalid_utf8,
                            sparenode::configuration::SourceLocation{0, 2, 3},
                            {}}});

    CHECK(diagnostic == "error: unable to open configuration file 'config/unsafe\\x1B\\x7F.conf'");
    CHECK(located_diagnostic ==
          "config/unsafe\\x1B\\x7F.conf:2:3: error: configuration input contains invalid UTF-8");
}

TEST_CASE("Configuration loader covers every lexer failure category",
          "[configuration][loader][errors]")
{
    using Code = sparenode::configuration::ConfigLexerErrorCode;
    struct Case
    {
        std::string name;
        std::string source;
        Code expected{};
    };

    std::vector<Case> cases;
    cases.push_back({"invalid UTF-8", std::string("server { ") + static_cast<char>(0xFF) + " }",
                     Code::invalid_utf8});
    cases.push_back({"embedded BOM", std::string("server { ") + "\xEF\xBB\xBF" + " }",
                     Code::unexpected_byte_order_mark});
    std::string embedded_null = "server { ";
    embedded_null.push_back('\0');
    embedded_null += " }";
    cases.push_back({"embedded null", std::move(embedded_null), Code::embedded_null});
    cases.push_back({"unexpected character", "server { @ }", Code::unexpected_character});
    cases.push_back(
        {"invalid escape", R"(server { bind "bad\q"; })", Code::invalid_escape_sequence});
    cases.push_back(
        {"unterminated string", R"(server { bind "unfinished)", Code::unterminated_string});
    cases.push_back(
        {"line break in string", "server { bind \"line\nbreak\"; }", Code::line_break_in_string});

    for (const auto &test_case : cases)
    {
        DYNAMIC_SECTION(test_case.name)
        {
            const sparenode::test::TemporaryDirectory directory("sparenode-config-lexer-error");
            const auto result = sparenode::configuration::ConfigLoader::load(
                write_config(directory, test_case.source));
            REQUIRE_FALSE(result.has_value());
            const auto *failure =
                std::get_if<sparenode::configuration::ConfigLexerError>(&result.error().failure);
            REQUIRE(failure != nullptr);
            CHECK(failure->code == test_case.expected);
        }
    }
}

TEST_CASE("Configuration loader covers every direct parser failure category",
          "[configuration][loader][errors]")
{
    using Code = sparenode::configuration::ConfigParserErrorCode;
    const std::vector<std::pair<std::string, Code>> cases{
        {"server { port 8080 }", Code::unexpected_token},
        {"server { port 18446744073709551616; }", Code::integer_out_of_range}};

    for (const auto &[source, expected] : cases)
    {
        const sparenode::test::TemporaryDirectory directory("sparenode-config-parser-error");
        const auto result =
            sparenode::configuration::ConfigLoader::load(write_config(directory, source));
        REQUIRE_FALSE(result.has_value());
        const auto *failure =
            std::get_if<sparenode::configuration::ConfigParserError>(&result.error().failure);
        REQUIRE(failure != nullptr);
        CHECK(failure->code == expected);
    }
}

TEST_CASE("Configuration loader covers every semantic validation failure category",
          "[configuration][loader][errors]")
{
    using Code = sparenode::configuration::ConfigValidationErrorCode;
    const sparenode::test::TemporaryDirectory directory("sparenode-config-validation-error");
    const std::string root = directory.path().generic_string();
    const std::string valid_location = make_location("/api/Documents", "path \"" + root + "\";");
    const std::string nested_location =
        make_location("/api/Documents/private", "path \"" + root + "\";");
    const std::vector<std::pair<Code, std::string>> cases{
        {Code::duplicate_server_directive, make_config("port 8080;\nport 8081;\n", valid_location)},
        {Code::missing_location, make_config("", "")},
        {Code::invalid_bind_address, make_config("bind \"localhost\";\n", valid_location)},
        {Code::port_out_of_range, make_config("port 0;\n", valid_location)},
        {Code::missing_worker_threads, make_config("multithreading true;\n", valid_location)},
        {Code::unexpected_worker_threads, make_config("worker_threads 4;\n", valid_location)},
        {Code::worker_threads_out_of_range,
         make_config("multithreading true;\nworker_threads 65;\n", valid_location)},
        {Code::invalid_log_level, make_config("log_level \"trace\";\n", valid_location)},
        {Code::invalid_location_path, make_config("", make_location("", "path \"" + root + "\";"))},
        {Code::conflicting_location_path, make_config("", valid_location + nested_location)},
        {Code::duplicate_location_directive,
         make_config("", make_location("/api/Documents",
                                       "path \"" + root + "\";\npath \"" + root + "\";"))},
        {Code::missing_location_root,
         make_config("", make_location("/api/Documents", "read true;"))},
        {Code::invalid_location_root,
         make_config("", make_location("/api/Documents",
                                       "path \"" + (directory.path() / "missing").generic_string() +
                                           "\";"))}};

    for (const auto &[expected, source] : cases)
    {
        DYNAMIC_SECTION(static_cast<std::uint32_t>(expected))
        {
            const auto result =
                sparenode::configuration::ConfigLoader::load(write_config(directory, source));
            REQUIRE_FALSE(result.has_value());
            CHECK(has_validation_error(result.error(), expected));
        }
    }
}
