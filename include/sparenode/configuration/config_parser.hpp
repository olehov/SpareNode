#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/parsed_config.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies the parser stage that rejected a token stream.
enum class ConfigParserErrorCode : std::uint8_t
{
    lexical_error,        ///< The underlying lexer could not produce the next token.
    unexpected_token,     ///< A produced token does not match the grammar position.
    integer_out_of_range, ///< An integer literal cannot fit the parser's unsigned type.
};

/// @brief Describes the grammatical element expected at a parser failure.
enum class ConfigParserExpectation : std::uint8_t
{
    server_keyword,  ///< Lowercase `server`.
    left_brace,      ///< `{`.
    right_brace,     ///< `}`.
    semicolon,       ///< `;`.
    server_item,     ///< A server directive or `share` block.
    share_item,      ///< A share directive.
    string_literal,  ///< A quoted string value.
    integer_literal, ///< An unsigned decimal integer value.
    boolean_literal, ///< Lowercase `true` or `false`.
    end_of_input,    ///< No further token.
};

/// @brief Describes a structured lexical or grammatical parsing failure.
struct ConfigParserError
{
    ConfigParserErrorCode code{};                     ///< Stable failure category.
    SourceLocation location;                          ///< Offending token or character.
    std::optional<ConfigParserExpectation> expected;  ///< Expected grammar element.
    std::optional<ConfigTokenKind> actual_token_kind; ///< Token encountered, if produced.
    std::optional<std::string> actual_identifier;     ///< Unknown identifier spelling, if safe.
    std::optional<ConfigLexerError> lexer_error;      ///< Original lexical failure, if any.
};

/// @brief Converts a configuration token stream into a typed syntactic representation.
class ConfigParser final
{
  public:
    /// @brief Parses the complete token stream produced by a lexer.
    /// @param[in] lexer Lexer positioned before its first token.
    /// @return Parsed configuration or the first structured parsing failure.
    [[nodiscard]] static Result<ParsedConfiguration, ConfigParserError> parse(ConfigLexer lexer);
};

/// @brief Returns a concise description of a parser failure category.
/// @param[in] code Portable failure category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(ConfigParserErrorCode code) noexcept;

/// @brief Returns a concise description of expected configuration syntax.
/// @param[in] expectation Grammar element to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(ConfigParserExpectation expectation) noexcept;

} // namespace sparenode::configuration
