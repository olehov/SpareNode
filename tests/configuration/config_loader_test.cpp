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
#include "sparenode/configuration/runtime/share_permissions.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "support/temporary_directory.hpp"

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
           "share \"Documents\" {\n"
           "path \"" +
           shared_root.generic_string() + "\";\nread false;\nwrite true;\ndelete true;\n}\n}";
}

/// @brief Wraps server directives and share source in a complete configuration document.
[[nodiscard]] std::string make_config(const std::string_view server_directives,
                                      const std::string_view shares)
{
    return "server {\n" + std::string(server_directives) + std::string(shares) + "\n}";
}

/// @brief Creates one share block with caller-selected name and body.
[[nodiscard]] std::string make_share(const std::string_view name, const std::string_view body)
{
    return "share \"" + std::string(name) + "\" {\n" + std::string(body) + "\n}\n";
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
    REQUIRE(result->servers.size() == 1);
    const auto &server = result->servers.front();
    CHECK(server.endpoint.address == "127.0.0.1");
    CHECK(server.endpoint.port == 8181);
    CHECK(server.effective_worker_count() == 3);
    CHECK(server.minimum_log_severity == sparenode::logging::LogSeverity::warning);
    REQUIRE(server.shares.size() == 1);
    CHECK(server.shares.front().root.path() == std::filesystem::canonical(directory.path()));
    CHECK(server.shares.front().permissions ==
          sparenode::configuration::runtime::SharePermissions{false, true, true});
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
    CHECK(diagnostic.find("required share block is missing") != std::string::npos);
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
    const std::string valid_share = make_share("Documents", "path \"" + root + "\";");
    const std::string second_share = make_share("Backup", "path \"" + root + "\";");
    const std::vector<std::pair<Code, std::string>> cases{
        {Code::duplicate_server_directive, make_config("port 8080;\nport 8081;\n", valid_share)},
        {Code::missing_share, make_config("", "")},
        {Code::multiple_shares, make_config("", valid_share + second_share)},
        {Code::invalid_bind_address, make_config("bind \"localhost\";\n", valid_share)},
        {Code::port_out_of_range, make_config("port 0;\n", valid_share)},
        {Code::missing_worker_threads, make_config("multithreading true;\n", valid_share)},
        {Code::unexpected_worker_threads, make_config("worker_threads 4;\n", valid_share)},
        {Code::worker_threads_out_of_range,
         make_config("multithreading true;\nworker_threads 65;\n", valid_share)},
        {Code::invalid_log_level, make_config("log_level \"trace\";\n", valid_share)},
        {Code::empty_share_name, make_config("", make_share("", "path \"" + root + "\";"))},
        {Code::duplicate_share_name, make_config("", valid_share + valid_share)},
        {Code::duplicate_share_directive,
         make_config("",
                     make_share("Documents", "path \"" + root + "\";\npath \"" + root + "\";"))},
        {Code::missing_share_path, make_config("", make_share("Documents", "read true;"))},
        {Code::invalid_share_path,
         make_config(
             "", make_share("Documents",
                            "path \"" + (directory.path() / "missing").generic_string() + "\";"))}};

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
