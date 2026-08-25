#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration
{

/// @brief Identifies one lexical element of the `spnode.conf` language.
enum class ConfigTokenKind : std::uint8_t
{
    identifier,      ///< Directive, block, or otherwise unclassified identifier.
    string_literal,  ///< Double-quoted string with a separately decoded value.
    integer_literal, ///< Unsigned sequence of ASCII decimal digits.
    boolean_literal, ///< Lowercase `true` or `false` keyword.
    left_brace,      ///< `{` block opener.
    right_brace,     ///< `}` block closer.
    semicolon,       ///< `;` statement terminator.
    end_of_input,    ///< Explicit terminal token.
};

/// @brief Describes one token without assigning configuration semantics.
///
/// `lexeme` references the original input passed to `ConfigLexer::create()` and
/// remains valid only while that input remains alive and unmodified. Only string
/// tokens populate `decoded_string`; every other token keeps it empty.
struct ConfigToken
{
    ConfigTokenKind kind{};                    ///< Portable token category.
    std::string_view lexeme;                   ///< Exact bytes occupied in source input.
    std::optional<std::string> decoded_string; ///< Decoded string value when applicable.
    SourceLocation location;                   ///< Position of the token's first character.
};

} // namespace sparenode::configuration
