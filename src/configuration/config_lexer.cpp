#include "sparenode/configuration/config_lexer.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "sparenode/configuration/detail/config_lexer_constants.hpp"

namespace sparenode::configuration
{
namespace
{

namespace lexer_constants = detail::config_lexer_constants;

/// @brief Converts a potentially signed source character into its byte value.
/// @param[in] character Source character to convert without sign extension.
/// @return Equivalent unsigned byte value.
[[nodiscard]] constexpr unsigned char to_unsigned_byte(const char character) noexcept
{
    return static_cast<unsigned char>(character);
}

/// @brief Reports whether an ASCII byte can begin an identifier.
/// @param[in] byte Byte to classify.
/// @return `true` for an ASCII letter or underscore.
[[nodiscard]] constexpr bool is_identifier_start(const unsigned char byte) noexcept
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_';
}

/// @brief Reports whether an ASCII byte can continue an identifier.
/// @param[in] byte Byte to classify.
/// @return `true` for an identifier-start byte, digit, or hyphen.
[[nodiscard]] constexpr bool is_identifier_continuation(const unsigned char byte) noexcept
{
    return is_identifier_start(byte) || (byte >= '0' && byte <= '9') || byte == '-';
}

/// @brief Reports whether input begins with a UTF-8 byte-order mark at an offset.
/// @param[in] input Complete source bytes.
/// @param[in] offset Candidate byte offset.
/// @return `true` when all three BOM bytes are present.
[[nodiscard]] bool has_byte_order_mark(const std::string_view input,
                                       const std::size_t offset) noexcept
{
    if (input.size() - std::min(offset, input.size()) <
        lexer_constants::utf8_byte_order_mark.size())
    {
        return false;
    }

    return to_unsigned_byte(input[offset]) == lexer_constants::utf8_byte_order_mark[0] &&
           to_unsigned_byte(input[offset + 1]) == lexer_constants::utf8_byte_order_mark[1] &&
           to_unsigned_byte(input[offset + 2]) == lexer_constants::utf8_byte_order_mark[2];
}

/// @brief Copies a bounded source fragment for error diagnostics.
/// @param[in] input Complete source bytes.
/// @param[in] offset First offending byte.
/// @param[in] length Desired byte count.
/// @return Owned context containing at most five bytes.
[[nodiscard]] std::string make_context(const std::string_view input, const std::size_t offset,
                                       const std::size_t length = 1)
{
    return std::string(
        input.substr(offset, std::min(length, lexer_constants::maximum_error_context_bytes)));
}

} // namespace

Result<ConfigLexer, ConfigLexerError> ConfigLexer::create(const std::string_view input)
{
    if (input.size() > max_input_size_bytes)
    {
        return unexpected(ConfigLexerError{ConfigLexerErrorCode::input_too_large, SourceLocation{},
                                           std::to_string(input.size()) + " bytes"});
    }

    return ConfigLexer(input);
}

ConfigLexer::ConfigLexer(const std::string_view input) noexcept : input_(input)
{
    if (has_byte_order_mark(input_, 0))
    {
        offset_ = lexer_constants::utf8_byte_order_mark.size();
    }
}

bool ConfigLexer::at_end() const noexcept
{
    return offset_ == input_.size();
}

SourceLocation ConfigLexer::location() const noexcept
{
    return SourceLocation{offset_, line_, column_};
}

void ConfigLexer::advance_ascii() noexcept
{
    assert(!at_end());
    const char current = input_[offset_];
    ++offset_;

    if (current == '\r')
    {
        if (!at_end() && input_[offset_] == '\n')
        {
            ++offset_;
        }
        ++line_;
        column_ = 1;
        return;
    }
    if (current == '\n')
    {
        ++line_;
        column_ = 1;
        return;
    }

    ++column_;
}

std::size_t ConfigLexer::utf8_sequence_length() const noexcept
{
    assert(!at_end());
    const unsigned char leading = to_unsigned_byte(input_[offset_]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum_code_point = 0;

    if ((leading & lexer_constants::utf8_two_byte_prefix_mask) ==
        lexer_constants::utf8_two_byte_prefix)
    {
        length = 2;
        code_point = leading & lexer_constants::utf8_two_byte_payload_mask;
        minimum_code_point = lexer_constants::utf8_two_byte_minimum_code_point;
    }
    else if ((leading & lexer_constants::utf8_three_byte_prefix_mask) ==
             lexer_constants::utf8_three_byte_prefix)
    {
        length = 3;
        code_point = leading & lexer_constants::utf8_three_byte_payload_mask;
        minimum_code_point = lexer_constants::utf8_three_byte_minimum_code_point;
    }
    else if ((leading & lexer_constants::utf8_four_byte_prefix_mask) ==
             lexer_constants::utf8_four_byte_prefix)
    {
        length = 4;
        code_point = leading & lexer_constants::utf8_four_byte_payload_mask;
        minimum_code_point = lexer_constants::utf8_four_byte_minimum_code_point;
    }
    else
    {
        return 0;
    }

    if (length > input_.size() - offset_)
    {
        return 0;
    }

    for (std::size_t index = 1; index < length; ++index)
    {
        const unsigned char continuation = to_unsigned_byte(input_[offset_ + index]);
        if ((continuation & lexer_constants::utf8_continuation_prefix_mask) !=
            lexer_constants::utf8_continuation_prefix)
        {
            return 0;
        }
        code_point = (code_point << lexer_constants::utf8_continuation_payload_bits) |
                     (continuation & lexer_constants::utf8_continuation_payload_mask);
    }

    if (code_point < minimum_code_point ||
        (code_point >= lexer_constants::unicode_first_surrogate &&
         code_point <= lexer_constants::unicode_last_surrogate) ||
        code_point > lexer_constants::unicode_maximum_code_point)
    {
        return 0;
    }

    return length;
}

void ConfigLexer::advance_utf8(const std::size_t sequence_length) noexcept
{
    offset_ += sequence_length;
    ++column_;
}

std::optional<ConfigLexerError> ConfigLexer::skip_trivia()
{
    while (!at_end())
    {
        const unsigned char byte = to_unsigned_byte(input_[offset_]);
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
        {
            advance_ascii();
            continue;
        }

        if (byte != '#')
        {
            return std::nullopt;
        }

        advance_ascii();
        while (!at_end() && input_[offset_] != '\r' && input_[offset_] != '\n')
        {
            const unsigned char comment_byte = to_unsigned_byte(input_[offset_]);
            if (comment_byte == 0)
            {
                auto error = ConfigLexerError{ConfigLexerErrorCode::embedded_null, location(),
                                              make_context(input_, offset_)};
                return error;
            }
            if (has_byte_order_mark(input_, offset_))
            {
                auto error = ConfigLexerError{
                    ConfigLexerErrorCode::unexpected_byte_order_mark, location(),
                    make_context(input_, offset_, lexer_constants::utf8_byte_order_mark.size())};
                return error;
            }
            if (comment_byte < lexer_constants::utf8_first_non_ascii_byte)
            {
                advance_ascii();
                continue;
            }

            const auto length = utf8_sequence_length();
            if (length == 0)
            {
                auto error = ConfigLexerError{ConfigLexerErrorCode::invalid_utf8, location(),
                                              make_context(input_, offset_)};
                return error;
            }
            advance_utf8(length);
        }
    }

    return std::nullopt;
}

ConfigToken ConfigLexer::lex_identifier()
{
    const auto start = offset_;
    const auto start_location = location();
    do
    {
        advance_ascii();
    } while (!at_end() && is_identifier_continuation(to_unsigned_byte(input_[offset_])));

    const auto lexeme = input_.substr(start, offset_ - start);
    const auto kind = lexeme == "true" || lexeme == "false" ? ConfigTokenKind::boolean_literal
                                                            : ConfigTokenKind::identifier;
    return ConfigToken{kind, lexeme, std::nullopt, start_location};
}

ConfigToken ConfigLexer::lex_integer()
{
    const auto start = offset_;
    const auto start_location = location();
    do
    {
        advance_ascii();
    } while (!at_end() && input_[offset_] >= '0' && input_[offset_] <= '9');

    return ConfigToken{ConfigTokenKind::integer_literal, input_.substr(start, offset_ - start),
                       std::nullopt, start_location};
}

Result<ConfigToken, ConfigLexerError> ConfigLexer::lex_string()
{
    const auto start = offset_;
    const auto start_location = location();
    std::string decoded;
    advance_ascii();

    while (!at_end() && input_[offset_] != '"')
    {
        auto error = consume_string_character(decoded);
        if (error)
        {
            return fail(std::move(*error));
        }
    }

    if (at_end())
    {
        return fail(ConfigLexerError{ConfigLexerErrorCode::unterminated_string, start_location,
                                     make_context(input_, start)});
    }

    advance_ascii();
    return ConfigToken{ConfigTokenKind::string_literal, input_.substr(start, offset_ - start),
                       std::move(decoded), start_location};
}

std::optional<ConfigLexerError> ConfigLexer::consume_string_character(std::string &decoded)
{
    const unsigned char byte = to_unsigned_byte(input_[offset_]);
    if (byte == '\r' || byte == '\n')
    {
        return ConfigLexerError{ConfigLexerErrorCode::line_break_in_string, location(),
                                make_context(input_, offset_)};
    }
    if (byte == 0)
    {
        return ConfigLexerError{ConfigLexerErrorCode::embedded_null, location(),
                                make_context(input_, offset_)};
    }
    if (has_byte_order_mark(input_, offset_))
    {
        return ConfigLexerError{
            ConfigLexerErrorCode::unexpected_byte_order_mark, location(),
            make_context(input_, offset_, lexer_constants::utf8_byte_order_mark.size())};
    }
    if (byte == '\\')
    {
        return consume_escape(decoded);
    }
    if (byte < lexer_constants::utf8_first_non_ascii_byte)
    {
        decoded.push_back(static_cast<char>(byte));
        advance_ascii();
        return std::nullopt;
    }

    const auto length = utf8_sequence_length();
    if (length == 0)
    {
        return ConfigLexerError{ConfigLexerErrorCode::invalid_utf8, location(),
                                make_context(input_, offset_)};
    }
    decoded.append(input_.substr(offset_, length));
    advance_utf8(length);
    return std::nullopt;
}

std::optional<ConfigLexerError> ConfigLexer::consume_escape(std::string &decoded)
{
    const auto escape_location = location();
    advance_ascii();
    if (at_end())
    {
        return ConfigLexerError{ConfigLexerErrorCode::invalid_escape_sequence, escape_location,
                                "\\"};
    }

    const unsigned char byte = to_unsigned_byte(input_[offset_]);
    if (byte == 0)
    {
        return ConfigLexerError{ConfigLexerErrorCode::embedded_null, location(),
                                make_context(input_, offset_)};
    }
    if (byte == '\r' || byte == '\n')
    {
        return ConfigLexerError{ConfigLexerErrorCode::line_break_in_string, location(),
                                make_context(input_, offset_)};
    }
    if (has_byte_order_mark(input_, offset_))
    {
        return ConfigLexerError{
            ConfigLexerErrorCode::unexpected_byte_order_mark, location(),
            make_context(input_, offset_, lexer_constants::utf8_byte_order_mark.size())};
    }
    if (byte >= lexer_constants::utf8_first_non_ascii_byte && utf8_sequence_length() == 0)
    {
        return ConfigLexerError{ConfigLexerErrorCode::invalid_utf8, location(),
                                make_context(input_, offset_)};
    }

    const char escaped = input_[offset_];
    switch (escaped)
    {
    case '"':
        decoded.push_back('"');
        break;
    case '\\':
        decoded.push_back('\\');
        break;
    case 'n':
        decoded.push_back('\n');
        break;
    case 'r':
        decoded.push_back('\r');
        break;
    case 't':
        decoded.push_back('\t');
        break;
    default:
    {
        const std::size_t escaped_length = byte < lexer_constants::utf8_first_non_ascii_byte
                                               ? std::size_t{1}
                                               : utf8_sequence_length();
        return ConfigLexerError{
            ConfigLexerErrorCode::invalid_escape_sequence, escape_location,
            make_context(input_, escape_location.byte_offset, std::size_t{1} + escaped_length)};
    }
    }
    advance_ascii();
    return std::nullopt;
}

ConfigToken ConfigLexer::lex_punctuation(const ConfigTokenKind kind)
{
    const auto start = offset_;
    const auto start_location = location();
    advance_ascii();
    return ConfigToken{kind, input_.substr(start, 1), std::nullopt, start_location};
}

Result<ConfigToken, ConfigLexerError> ConfigLexer::fail(ConfigLexerError error)
{
    terminal_error_ = std::move(error);
    return unexpected(*terminal_error_);
}

Result<ConfigToken, ConfigLexerError> ConfigLexer::next()
{
    if (terminal_error_)
    {
        return unexpected(*terminal_error_);
    }

    auto trivia_error = skip_trivia();
    if (trivia_error)
    {
        return fail(std::move(*trivia_error));
    }
    if (at_end())
    {
        return ConfigToken{ConfigTokenKind::end_of_input, input_.substr(offset_, 0), std::nullopt,
                           location()};
    }

    const unsigned char byte = to_unsigned_byte(input_[offset_]);
    if (is_identifier_start(byte))
    {
        return lex_identifier();
    }
    if (byte >= '0' && byte <= '9')
    {
        return lex_integer();
    }

    switch (byte)
    {
    case '"':
        return lex_string();
    case '{':
        return lex_punctuation(ConfigTokenKind::left_brace);
    case '}':
        return lex_punctuation(ConfigTokenKind::right_brace);
    case ';':
        return lex_punctuation(ConfigTokenKind::semicolon);
    case 0:
        return fail(ConfigLexerError{ConfigLexerErrorCode::embedded_null, location(),
                                     make_context(input_, offset_)});
    default:
        break;
    }

    if (has_byte_order_mark(input_, offset_))
    {
        return fail(ConfigLexerError{
            ConfigLexerErrorCode::unexpected_byte_order_mark, location(),
            make_context(input_, offset_, lexer_constants::utf8_byte_order_mark.size())});
    }
    if (byte >= lexer_constants::utf8_first_non_ascii_byte && utf8_sequence_length() == 0)
    {
        return fail(ConfigLexerError{ConfigLexerErrorCode::invalid_utf8, location(),
                                     make_context(input_, offset_)});
    }

    const std::size_t context_length =
        byte < lexer_constants::utf8_first_non_ascii_byte ? std::size_t{1} : utf8_sequence_length();
    return fail(ConfigLexerError{ConfigLexerErrorCode::unexpected_character, location(),
                                 make_context(input_, offset_, context_length)});
}

const char *to_string(const ConfigLexerErrorCode code) noexcept
{
    switch (code)
    {
    case ConfigLexerErrorCode::input_too_large:
        return "configuration input exceeds the lexer size limit";
    case ConfigLexerErrorCode::invalid_utf8:
        return "configuration input contains invalid UTF-8";
    case ConfigLexerErrorCode::unexpected_byte_order_mark:
        return "UTF-8 byte-order mark is only allowed at file start";
    case ConfigLexerErrorCode::embedded_null:
        return "configuration input contains an embedded null byte";
    case ConfigLexerErrorCode::unexpected_character:
        return "unexpected character in configuration input";
    case ConfigLexerErrorCode::invalid_escape_sequence:
        return "invalid escape sequence in string literal";
    case ConfigLexerErrorCode::unterminated_string:
        return "unterminated string literal";
    case ConfigLexerErrorCode::line_break_in_string:
        return "raw line break in string literal";
    }

    return "unknown configuration lexer error";
}

} // namespace sparenode::configuration
