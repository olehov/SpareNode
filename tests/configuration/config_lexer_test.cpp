#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/config_lexer.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Tokenizes complete input and requires every lexer operation to succeed.
/// @param[in] input Source that remains alive while returned token views are inspected.
/// @return Tokens through and including explicit end-of-input.
[[nodiscard]] std::vector<sparenode::configuration::ConfigToken>
require_tokens(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());
    auto lexer = std::move(lexer_result).value();

    std::vector<sparenode::configuration::ConfigToken> tokens;
    while (tokens.empty() ||
           tokens.back().kind != sparenode::configuration::ConfigTokenKind::end_of_input)
    {
        auto token_result = lexer.next();
        REQUIRE(token_result.has_value());
        tokens.push_back(std::move(token_result).value());
    }
    return tokens;
}

/// @brief Creates a lexer and returns its first failure.
/// @param[in] input Malformed source expected to fail before end-of-input.
/// @return Structured lexical error.
[[nodiscard]] sparenode::configuration::ConfigLexerError
require_lexer_error(const std::string_view input)
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create(input);
    REQUIRE(lexer_result.has_value());
    auto lexer = std::move(lexer_result).value();

    while (true)
    {
        auto token_result = lexer.next();
        if (!token_result)
        {
            return token_result.error();
        }
        REQUIRE(token_result->kind != sparenode::configuration::ConfigTokenKind::end_of_input);
    }
}

} // namespace

TEST_CASE("Configuration lexer represents empty input deterministically", "[configuration][lexer]")
{
    const auto tokens = require_tokens("");

    REQUIRE(tokens.size() == 1);
    CHECK(tokens.front().kind == sparenode::configuration::ConfigTokenKind::end_of_input);
    CHECK(tokens.front().lexeme.empty());
    CHECK(tokens.front().location == sparenode::configuration::SourceLocation{0, 1, 1});
}

TEST_CASE("Configuration lexer reduces whitespace-only input to end-of-input",
          "[configuration][lexer]")
{
    constexpr std::string_view input = " \t\r\n\n";
    const auto tokens = require_tokens(input);

    REQUIRE(tokens.size() == 1);
    CHECK(tokens.front().kind == sparenode::configuration::ConfigTokenKind::end_of_input);
    CHECK(tokens.front().location == sparenode::configuration::SourceLocation{5, 3, 1});
}

TEST_CASE("Configuration lexer consumes whitespace comments and a leading BOM",
          "[configuration][lexer]")
{
    const std::string input = "\xEF\xBB\xBF # коментар\r\n\tserver";
    const auto tokens = require_tokens(input);

    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].kind == sparenode::configuration::ConfigTokenKind::identifier);
    CHECK(tokens[0].lexeme == "server");
    CHECK(tokens[0].location == sparenode::configuration::SourceLocation{25, 2, 2});
    CHECK(tokens[1].location == sparenode::configuration::SourceLocation{31, 2, 8});
}

TEST_CASE("Configuration lexer tokenizes the complete version one example",
          "[configuration][lexer]")
{
    constexpr std::string_view input = R"(
server {
    bind "0.0.0.0";
    port 8080;
    multithreading true;
    worker_threads 4;
    log_level "info";
    share "Documents" {
        path "/home/user/Documents";
        read true;
        write false;
        delete false;
    }
}
)";

    const auto tokens = require_tokens(input);
    REQUIRE(tokens.size() == 35);
    CHECK(tokens[0].lexeme == "server");
    CHECK(tokens[1].kind == sparenode::configuration::ConfigTokenKind::left_brace);
    CHECK(tokens[3].kind == sparenode::configuration::ConfigTokenKind::string_literal);
    CHECK(sparenode::test::require_optional(tokens[3].decoded_string) == "0.0.0.0");
    CHECK(tokens[6].kind == sparenode::configuration::ConfigTokenKind::integer_literal);
    CHECK(tokens[9].kind == sparenode::configuration::ConfigTokenKind::boolean_literal);
    CHECK(tokens[34].kind == sparenode::configuration::ConfigTokenKind::end_of_input);
}

TEST_CASE("Configuration lexer decodes every supported string escape exactly once",
          "[configuration][lexer]")
{
    constexpr std::string_view input = R"("quote\" slash\\ newline\n return\r tab\t")";
    const auto tokens = require_tokens(input);

    REQUIRE(tokens.size() == 2);
    CHECK(sparenode::test::require_optional(tokens[0].decoded_string) ==
          "quote\" slash\\ newline\n return\r tab\t");
    CHECK(tokens[0].lexeme == input);
}

TEST_CASE("Configuration lexer preserves UTF-8 Linux and escaped Windows paths",
          "[configuration][lexer]")
{
    constexpr std::string_view input =
        "\"/home/user/\xD0\x94\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB8\" "
        "\"D:\\\\Shared\\\\Documents\"";
    const auto tokens = require_tokens(input);

    REQUIRE(tokens.size() == 3);
    CHECK(sparenode::test::require_optional(tokens[0].decoded_string) ==
          "/home/user/\xD0\x94\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB8");
    CHECK(sparenode::test::require_optional(tokens[1].decoded_string) == "D:\\Shared\\Documents");
}

TEST_CASE("Configuration lexer tracks token positions across LF and CRLF", "[configuration][lexer]")
{
    constexpr std::string_view input = "# first\r\nserver {\n\tport 8080;\r\n}";
    const auto tokens = require_tokens(input);

    REQUIRE(tokens.size() == 7);
    CHECK(tokens[0].location == sparenode::configuration::SourceLocation{9, 2, 1});
    CHECK(tokens[2].location == sparenode::configuration::SourceLocation{19, 3, 2});
    CHECK(tokens[3].location == sparenode::configuration::SourceLocation{24, 3, 7});
    CHECK(tokens[5].location == sparenode::configuration::SourceLocation{31, 4, 1});
    CHECK(tokens[6].location == sparenode::configuration::SourceLocation{32, 4, 2});
}

TEST_CASE("Configuration lexer rejects malformed strings with structured locations",
          "[configuration][lexer]")
{
    SECTION("unterminated")
    {
        const auto error = require_lexer_error("path \"missing");
        CHECK(error.code == sparenode::configuration::ConfigLexerErrorCode::unterminated_string);
        CHECK(error.location == sparenode::configuration::SourceLocation{5, 1, 6});
    }

    SECTION("unsupported escape")
    {
        const auto error = require_lexer_error("\"D:\\Shared\"");
        CHECK(error.code ==
              sparenode::configuration::ConfigLexerErrorCode::invalid_escape_sequence);
        CHECK(error.location == sparenode::configuration::SourceLocation{3, 1, 4});
        CHECK(error.context == "\\S");
    }

    SECTION("unsupported non-ASCII escape")
    {
        const std::string input = "\"\\\xF0\x9F\x98\x80\"";
        const auto error = require_lexer_error(input);
        CHECK(error.code ==
              sparenode::configuration::ConfigLexerErrorCode::invalid_escape_sequence);
        CHECK(error.location == sparenode::configuration::SourceLocation{1, 1, 2});
        CHECK(error.context == std::string("\\\xF0\x9F\x98\x80", 5));
    }

    SECTION("raw line break")
    {
        const auto error = require_lexer_error("\"first\nsecond\"");
        CHECK(error.code == sparenode::configuration::ConfigLexerErrorCode::line_break_in_string);
        CHECK(error.location == sparenode::configuration::SourceLocation{6, 1, 7});
    }
}

TEST_CASE("Configuration lexer rejects invalid source bytes", "[configuration][lexer]")
{
    SECTION("unexpected ASCII")
    {
        const auto error = require_lexer_error("server @");
        CHECK(error.code == sparenode::configuration::ConfigLexerErrorCode::unexpected_character);
        CHECK(error.location == sparenode::configuration::SourceLocation{7, 1, 8});
        CHECK(error.context == "@");
    }

    SECTION("embedded null")
    {
        const std::string input("server\0{}", 9);
        const auto error = require_lexer_error(input);
        CHECK(error.code == sparenode::configuration::ConfigLexerErrorCode::embedded_null);
        CHECK(error.location == sparenode::configuration::SourceLocation{6, 1, 7});
    }

    SECTION("invalid UTF-8")
    {
        const std::string input("\"\xC0\xAF\"", 4);
        const auto error = require_lexer_error(input);
        CHECK(error.code == sparenode::configuration::ConfigLexerErrorCode::invalid_utf8);
        CHECK(error.location == sparenode::configuration::SourceLocation{1, 1, 2});
    }

    SECTION("BOM after start")
    {
        const std::string input = "server \xEF\xBB\xBF{}";
        const auto error = require_lexer_error(input);
        CHECK(error.code ==
              sparenode::configuration::ConfigLexerErrorCode::unexpected_byte_order_mark);
        CHECK(error.location == sparenode::configuration::SourceLocation{7, 1, 8});
    }
}

TEST_CASE("Configuration lexer enforces its explicit input size limit", "[configuration][lexer]")
{
    const std::string accepted_input(sparenode::configuration::ConfigLexer::max_input_size_bytes,
                                     ' ');
    auto accepted_lexer_result = sparenode::configuration::ConfigLexer::create(accepted_input);
    REQUIRE(accepted_lexer_result.has_value());
    auto accepted_lexer = std::move(accepted_lexer_result).value();
    const auto end_result = accepted_lexer.next();
    REQUIRE(end_result.has_value());
    CHECK(end_result->kind == sparenode::configuration::ConfigTokenKind::end_of_input);

    const std::string rejected_input(
        sparenode::configuration::ConfigLexer::max_input_size_bytes + 1, ' ');
    const auto result = sparenode::configuration::ConfigLexer::create(rejected_input);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::configuration::ConfigLexerErrorCode::input_too_large);
    CHECK(result.error().location == sparenode::configuration::SourceLocation{0, 1, 1});
}

TEST_CASE("Configuration lexer preserves its first terminal error", "[configuration][lexer]")
{
    auto lexer_result = sparenode::configuration::ConfigLexer::create("@");
    REQUIRE(lexer_result.has_value());
    auto lexer = std::move(lexer_result).value();

    const auto first_result = lexer.next();
    const auto second_result = lexer.next();

    REQUIRE_FALSE(first_result.has_value());
    REQUIRE_FALSE(second_result.has_value());
    CHECK(first_result.error() == second_result.error());
}

TEST_CASE("Configuration lexer error descriptions are stable", "[configuration][lexer]")
{
    CHECK(std::string_view(sparenode::configuration::to_string(
              sparenode::configuration::ConfigLexerErrorCode::unterminated_string)) ==
          "unterminated string literal");
}
