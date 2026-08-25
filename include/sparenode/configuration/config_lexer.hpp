#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/configuration/config_token.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why `spnode.conf` tokenization could not continue.
enum class ConfigLexerErrorCode : std::uint8_t
{
    input_too_large,            ///< Input exceeds the documented lexer byte limit.
    invalid_utf8,               ///< Input contains a malformed UTF-8 sequence.
    unexpected_byte_order_mark, ///< A UTF-8 byte-order mark appears after file start.
    embedded_null,              ///< Input contains a null byte.
    unexpected_character,       ///< A valid character cannot begin any token.
    invalid_escape_sequence,    ///< A quoted string contains an unsupported escape.
    unterminated_string,        ///< End-of-input occurs before a closing quote.
    line_break_in_string,       ///< A raw line ending occurs inside a quoted string.
};

/// @brief Describes a terminal lexical failure with stable source coordinates.
struct ConfigLexerError
{
    ConfigLexerErrorCode code{}; ///< Portable failure category.
    SourceLocation location;     ///< Position associated with the failure.
    std::string context;         ///< Bounded offending bytes or diagnostic context.

    /// @brief Compares every structured error field.
    /// @param[in] lhs Left-hand error.
    /// @param[in] rhs Right-hand error.
    /// @return `true` when both errors describe the same failure.
    friend bool operator==(const ConfigLexerError &lhs, const ConfigLexerError &rhs) = default;
};

/// @brief Tokenizes UTF-8 `spnode.conf` input without parsing its block structure.
///
/// The lexer borrows its complete input. The caller must keep that input alive and
/// unmodified until the lexer and every returned token are no longer used. After a
/// lexical failure, subsequent `next()` calls return the same stored failure.
class ConfigLexer final
{
  public:
    /// @brief Maximum encoded configuration size accepted by the lexer.
    static constexpr std::size_t max_input_size_bytes = std::size_t{1024} * 1024;

    /// @brief Creates a lexer after enforcing the explicit input-size boundary.
    /// @param[in] input Complete UTF-8 source retained by the caller.
    /// @return A lexer positioned at the first token, or a structured size error.
    [[nodiscard]] static Result<ConfigLexer, ConfigLexerError> create(std::string_view input);

    /// @brief Returns the next token after consuming comments and whitespace.
    /// @return One token, including explicit end-of-input, or a terminal lexical error.
    [[nodiscard]] Result<ConfigToken, ConfigLexerError> next();

  private:
    /// @brief Starts at the first source byte, skipping an optional leading BOM.
    /// @param[in] input Complete source borrowed for this lexer's lifetime.
    explicit ConfigLexer(std::string_view input) noexcept;

    /// @brief Reports whether every source byte has been consumed.
    /// @return `true` at end-of-input.
    [[nodiscard]] bool at_end() const noexcept;

    /// @brief Returns the current source coordinates.
    /// @return Location corresponding to `offset_`.
    [[nodiscard]] SourceLocation location() const noexcept;

    /// @brief Advances over one ASCII character and updates line tracking.
    void advance_ascii() noexcept;

    /// @brief Validates and advances over one non-ASCII UTF-8 scalar.
    /// @return Scalar byte length, or zero for malformed UTF-8.
    [[nodiscard]] std::size_t utf8_sequence_length() const noexcept;

    /// @brief Advances over one already validated non-ASCII scalar.
    /// @param[in] sequence_length Valid UTF-8 byte length.
    void advance_utf8(std::size_t sequence_length) noexcept;

    /// @brief Consumes insignificant whitespace and comments.
    /// @return No value at the next token or end-of-input, otherwise a lexical error.
    [[nodiscard]] std::optional<ConfigLexerError> skip_trivia();

    /// @brief Reads an identifier or recognized boolean keyword.
    /// @return Identifier or boolean token.
    [[nodiscard]] ConfigToken lex_identifier();

    /// @brief Reads one unsigned ASCII decimal integer token.
    /// @return Integer token whose conversion is deferred to the parser.
    [[nodiscard]] ConfigToken lex_integer();

    /// @brief Reads and decodes one double-quoted string.
    /// @return String token or a terminal string/encoding error.
    [[nodiscard]] Result<ConfigToken, ConfigLexerError> lex_string();

    /// @brief Consumes one non-closing string character into decoded storage.
    /// @param[out] decoded String value receiving decoded source bytes.
    /// @return No value after consuming one character, otherwise a lexical error.
    [[nodiscard]] std::optional<ConfigLexerError> consume_string_character(std::string &decoded);

    /// @brief Decodes one escape beginning at the current backslash.
    /// @param[out] decoded String value receiving the escaped character.
    /// @return No value after consuming the escape, otherwise a lexical error.
    [[nodiscard]] std::optional<ConfigLexerError> consume_escape(std::string &decoded);

    /// @brief Creates a punctuation token and consumes its single byte.
    /// @param[in] kind Punctuation category associated with the current byte.
    /// @return Token at the pre-consumption location.
    [[nodiscard]] ConfigToken lex_punctuation(ConfigTokenKind kind);

    /// @brief Stores an error as the lexer's terminal state.
    /// @param[in] error Failure to preserve for subsequent calls.
    /// @return Failed token result containing the stored error.
    [[nodiscard]] Result<ConfigToken, ConfigLexerError> fail(ConfigLexerError error);

    std::string_view input_;                         ///< Complete borrowed UTF-8 source.
    std::size_t offset_{};                           ///< Current byte offset.
    std::size_t line_{1};                            ///< Current one-based line.
    std::size_t column_{1};                          ///< Current one-based scalar column.
    std::optional<ConfigLexerError> terminal_error_; ///< First terminal lexical failure.
};

/// @brief Returns a concise description of a configuration lexer failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(ConfigLexerErrorCode code) noexcept;

} // namespace sparenode::configuration
