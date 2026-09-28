#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "sparenode/configuration/config_parser.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Parses complete source and requires lexer construction and parsing to succeed.
/// @param[in] input Configuration source retained throughout the parse.
/// @return Owned parsed configuration.
[[nodiscard]] sparenode::configuration::ParsedConfiguration
require_configuration(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());

    auto parser_result =
        sparenode::configuration::ConfigParser::parse(std::move(lexer_result).value());
    REQUIRE(parser_result.has_value());
    return std::move(parser_result).value();
}

/// @brief Parses malformed source and requires a structured parser failure.
/// @param[in] input Configuration source retained throughout the parse.
/// @return First parser or wrapped lexer error.
[[nodiscard]] sparenode::configuration::ConfigParserError
require_parser_error(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());

    auto parser_result =
        sparenode::configuration::ConfigParser::parse(std::move(lexer_result).value());
    REQUIRE(!parser_result.has_value());
    return parser_result.error();
}

} // namespace

TEST_CASE("Configuration receive timeouts require server-scoped unsigned integers",
          "[configuration][parser][timeout]")
{
    const std::string directive =
        GENERATE("header_timeout_ms", "body_timeout_ms", "request_timeout_ms");
    SECTION("incorrect scalar syntax")
    {
        const std::string value = GENERATE("true", "\"1000\"", "-1", "1.5", "10ms");
        const auto error = require_parser_error("server { " + directive + " " + value + "; }");
        CHECK(error.code != sparenode::configuration::ConfigParserErrorCode::integer_out_of_range);
    }
    SECTION("wrong block")
    {
        const auto error =
            require_parser_error("server { location \"docs\" { " + directive + " 1000; } }");
        CHECK(error.code == sparenode::configuration::ConfigParserErrorCode::unexpected_token);
    }
}

TEST_CASE("Configuration parser accepts an empty server block", "[configuration][parser]")
{
    const auto configuration = require_configuration("server {}");

    CHECK(configuration.server.location == sparenode::configuration::SourceLocation{0, 1, 1});
    CHECK(configuration.server.closing_brace_location ==
          sparenode::configuration::SourceLocation{8, 1, 9});
    CHECK(configuration.server.directives.empty());
    CHECK(configuration.server.locations.empty());
    CHECK(configuration.end_of_input_location ==
          sparenode::configuration::SourceLocation{9, 1, 10});
}

TEST_CASE("Configuration parser creates typed values for the complete grammar",
          "[configuration][parser]")
{
    constexpr std::string_view input = R"(server {
    bind "127.0.0.1";
    port 8080;
    multithreading true;
    worker_threads 4;
    log_level "debug";
    location "/api/Documents" {
        path "/srv/Documents";
        read true;
        write false;
        delete false;
    }
})";

    const auto configuration = require_configuration(input);
    REQUIRE(configuration.server.directives.size() == 5);
    CHECK(configuration.server.directives[0].kind ==
          sparenode::configuration::directives::ServerDirectiveKind::bind);
    CHECK(std::get<std::string>(configuration.server.directives[0].value.scalar) == "127.0.0.1");
    CHECK(std::get<std::uint64_t>(configuration.server.directives[1].value.scalar) == 8080);
    CHECK(std::get<bool>(configuration.server.directives[2].value.scalar));
    CHECK(std::get<std::uint64_t>(configuration.server.directives[3].value.scalar) == 4);
    CHECK(std::get<std::string>(configuration.server.directives[4].value.scalar) == "debug");

    REQUIRE(configuration.server.locations.size() == 1);
    const auto &location = configuration.server.locations.front();
    CHECK(location.api_path == "/api/Documents");
    REQUIRE(location.directives.size() == 4);
    CHECK(location.directives[0].kind ==
          sparenode::configuration::directives::LocationDirectiveKind::path);
    CHECK(std::get<std::string>(location.directives[0].value.scalar) == "/srv/Documents");
    CHECK(std::get<bool>(location.directives[1].value.scalar));
    CHECK_FALSE(std::get<bool>(location.directives[2].value.scalar));
    CHECK_FALSE(std::get<bool>(location.directives[3].value.scalar));
}

TEST_CASE("Configuration parser preserves repeated syntax for semantic validation",
          "[configuration][parser]")
{
    constexpr std::string_view input = R"(server {
    port 8080;
    port 9090;
    location "/api/First" { path "/first"; }
    location "/api/Second" { path "/second"; }
})";

    const auto configuration = require_configuration(input);

    REQUIRE(configuration.server.directives.size() == 2);
    CHECK(std::get<std::uint64_t>(configuration.server.directives[0].value.scalar) == 8080);
    CHECK(std::get<std::uint64_t>(configuration.server.directives[1].value.scalar) == 9090);
    REQUIRE(configuration.server.locations.size() == 2);
    CHECK(configuration.server.locations[0].api_path == "/api/First");
    CHECK(configuration.server.locations[1].api_path == "/api/Second");
}

TEST_CASE("Configuration parser rejects missing punctuation and delimiters",
          "[configuration][parser]")
{
    using sparenode::configuration::ConfigParserExpectation;
    using sparenode::configuration::ConfigTokenKind;

    SECTION("missing opening brace")
    {
        const auto error = require_parser_error("server port 8080;");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::left_brace);
        CHECK(sparenode::test::require_optional(error.actual_token_kind) ==
              ConfigTokenKind::identifier);
    }

    SECTION("missing semicolon")
    {
        const auto error = require_parser_error("server {\nport 8080\n}");
        CHECK(error.location == sparenode::configuration::SourceLocation{19, 3, 1});
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::semicolon);
        CHECK(sparenode::test::require_optional(error.actual_token_kind) ==
              ConfigTokenKind::right_brace);
    }

    SECTION("premature end of server block")
    {
        const auto error = require_parser_error("server {");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::right_brace);
        CHECK(sparenode::test::require_optional(error.actual_token_kind) ==
              ConfigTokenKind::end_of_input);
    }

    SECTION("premature end of location block")
    {
        const auto error = require_parser_error("server { location \"x\" {");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::right_brace);
        CHECK(sparenode::test::require_optional(error.actual_token_kind) ==
              ConfigTokenKind::end_of_input);
    }
}

TEST_CASE("Configuration parser rejects unexpected names nesting and value types",
          "[configuration][parser]")
{
    using sparenode::configuration::ConfigParserExpectation;
    using sparenode::configuration::ConfigTokenKind;

    SECTION("unknown top-level token")
    {
        const auto error = require_parser_error("location {}");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::server_keyword);
    }

    SECTION("unknown server directive")
    {
        const auto error = require_parser_error("server { listen 8080; }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::server_item);
        CHECK(sparenode::test::require_optional(error.actual_identifier) == "listen");
    }

    SECTION("server directive inside location")
    {
        const auto error = require_parser_error("server { location \"x\" { port 8; } }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::location_item);
    }

    SECTION("missing location name")
    {
        const auto error = require_parser_error("server { location { } }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::string_literal);
    }

    SECTION("wrong location directive value type")
    {
        const auto error = require_parser_error("server { location \"x\" { read \"yes\"; } }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::boolean_literal);
    }

    SECTION("unexpected nested block")
    {
        const auto error = require_parser_error("server { { } }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::server_item);
    }

    SECTION("wrong directive value type")
    {
        const auto error = require_parser_error("server { port \"8080\"; }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::integer_literal);
    }

    SECTION("missing directive value")
    {
        const auto error = require_parser_error("server { port; }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::integer_literal);
    }

    SECTION("tokens after server block")
    {
        const auto error = require_parser_error("server {} server {}");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::end_of_input);
    }
}

TEST_CASE("Configuration parser reports integer overflow without semantic range checks",
          "[configuration][parser]")
{
    const auto accepted = require_configuration("server { port 65536; }");
    CHECK(std::get<std::uint64_t>(accepted.server.directives.front().value.scalar) == 65536);

    const auto error = require_parser_error("server { port 18446744073709551616; }");
    CHECK(error.code == sparenode::configuration::ConfigParserErrorCode::integer_out_of_range);
    CHECK(error.location == sparenode::configuration::SourceLocation{14, 1, 15});
}

TEST_CASE("Configuration parser propagates terminal lexer errors", "[configuration][parser]")
{
    const auto error = require_parser_error("server { bind \"D:\\Location\"; }");

    CHECK(error.code == sparenode::configuration::ConfigParserErrorCode::lexical_error);
    const auto &lexer_error = sparenode::test::require_optional(error.lexer_error);
    CHECK(lexer_error.code ==
          sparenode::configuration::ConfigLexerErrorCode::invalid_escape_sequence);
    CHECK(error.location == lexer_error.location);
}

TEST_CASE("Configuration parser diagnostic descriptions are stable", "[configuration][parser]")
{
    CHECK(std::string_view(sparenode::configuration::to_string(
              sparenode::configuration::ConfigParserErrorCode::unexpected_token)) ==
          "unexpected configuration token");
    CHECK(std::string_view(sparenode::configuration::to_string(
              sparenode::configuration::ConfigParserExpectation::semicolon)) == "';'");
}
