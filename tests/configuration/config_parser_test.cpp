#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("Configuration parser accepts an empty server block", "[configuration][parser]")
{
    const auto configuration = require_configuration("server {}");

    CHECK(configuration.server.location == sparenode::configuration::SourceLocation{0, 1, 1});
    CHECK(configuration.server.closing_brace_location ==
          sparenode::configuration::SourceLocation{8, 1, 9});
    CHECK(configuration.server.directives.empty());
    CHECK(configuration.server.shares.empty());
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
    share "Documents" {
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

    REQUIRE(configuration.server.shares.size() == 1);
    const auto &share = configuration.server.shares.front();
    CHECK(share.name == "Documents");
    REQUIRE(share.directives.size() == 4);
    CHECK(share.directives[0].kind ==
          sparenode::configuration::directives::ShareDirectiveKind::path);
    CHECK(std::get<std::string>(share.directives[0].value.scalar) == "/srv/Documents");
    CHECK(std::get<bool>(share.directives[1].value.scalar));
    CHECK_FALSE(std::get<bool>(share.directives[2].value.scalar));
    CHECK_FALSE(std::get<bool>(share.directives[3].value.scalar));
}

TEST_CASE("Configuration parser preserves repeated syntax for semantic validation",
          "[configuration][parser]")
{
    constexpr std::string_view input = R"(server {
    port 8080;
    port 9090;
    share "First" { path "/first"; }
    share "Second" { path "/second"; }
})";

    const auto configuration = require_configuration(input);

    REQUIRE(configuration.server.directives.size() == 2);
    CHECK(std::get<std::uint64_t>(configuration.server.directives[0].value.scalar) == 8080);
    CHECK(std::get<std::uint64_t>(configuration.server.directives[1].value.scalar) == 9090);
    REQUIRE(configuration.server.shares.size() == 2);
    CHECK(configuration.server.shares[0].name == "First");
    CHECK(configuration.server.shares[1].name == "Second");
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

    SECTION("premature end of share block")
    {
        const auto error = require_parser_error("server { share \"x\" {");
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
        const auto error = require_parser_error("share {}");
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

    SECTION("server directive inside share")
    {
        const auto error = require_parser_error("server { share \"x\" { port 8; } }");
        CHECK(sparenode::test::require_optional(error.expected) ==
              ConfigParserExpectation::share_item);
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
    const auto error = require_parser_error("server { bind \"D:\\Share\"; }");

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
