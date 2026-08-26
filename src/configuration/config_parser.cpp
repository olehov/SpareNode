#include "sparenode/configuration/config_parser.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sparenode::configuration
{
namespace
{

/// @brief Owns mutable state for one complete, non-recursive parse.
class ParserState final
{
  public:
    /// @brief Creates parser state before the first token has been requested.
    /// @param[in] lexer Lexer whose token stream will be consumed exactly once.
    explicit ParserState(ConfigLexer lexer) : lexer_(std::move(lexer))
    {
    }

    /// @brief Parses the complete version-one configuration grammar.
    /// @return Owned syntax representation or the first parsing failure.
    [[nodiscard]] Result<ParsedConfiguration, ConfigParserError> parse()
    {
        if (auto error = advance(); error.has_value())
        {
            return unexpected(std::move(error).value());
        }

        if (!is_identifier("server"))
        {
            return unexpected(make_unexpected_error(ConfigParserExpectation::server_keyword));
        }
        ParsedServerBlock server;
        server.location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return unexpected(std::move(error).value());
        }
        if (auto error = consume(ConfigTokenKind::left_brace, ConfigParserExpectation::left_brace);
            error.has_value())
        {
            return unexpected(std::move(error).value());
        }

        while (current_.kind != ConfigTokenKind::right_brace &&
               current_.kind != ConfigTokenKind::end_of_input)
        {
            if (auto error = parse_server_item(server); error.has_value())
            {
                return unexpected(std::move(error).value());
            }
        }

        if (current_.kind != ConfigTokenKind::right_brace)
        {
            return unexpected(make_unexpected_error(ConfigParserExpectation::right_brace));
        }
        server.closing_brace_location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return unexpected(std::move(error).value());
        }
        if (current_.kind != ConfigTokenKind::end_of_input)
        {
            return unexpected(make_unexpected_error(ConfigParserExpectation::end_of_input));
        }

        return ParsedConfiguration{std::move(server), current_.location};
    }

  private:
    /// @brief Requests and stores the next token, preserving lexical failures.
    /// @return Success after advancing or a wrapped lexer error.
    [[nodiscard]] std::optional<ConfigParserError> advance()
    {
        auto token_result = lexer_.next();
        if (!token_result)
        {
            const auto &lexer_error = token_result.error();
            return ConfigParserError{ConfigParserErrorCode::lexical_error,
                                     lexer_error.location,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     lexer_error};
        }
        current_ = std::move(token_result).value();
        return std::nullopt;
    }

    /// @brief Reports whether the current token is a specific identifier.
    /// @param[in] name Exact case-sensitive identifier spelling.
    /// @return `true` when the current token matches `name`.
    [[nodiscard]] bool is_identifier(const std::string_view name) const noexcept
    {
        return current_.kind == ConfigTokenKind::identifier && current_.lexeme == name;
    }

    /// @brief Creates an unexpected-token error from the current parser position.
    /// @param[in] expected Grammar element required at this position.
    /// @return Structured syntax failure containing expected and actual token kinds.
    [[nodiscard]] ConfigParserError
    make_unexpected_error(const ConfigParserExpectation expected) const
    {
        std::optional<std::string> actual_identifier;
        if (current_.kind == ConfigTokenKind::identifier)
        {
            actual_identifier = current_.lexeme;
        }
        return ConfigParserError{ConfigParserErrorCode::unexpected_token,
                                 current_.location,
                                 expected,
                                 current_.kind,
                                 std::move(actual_identifier),
                                 std::nullopt};
    }

    /// @brief Requires one token kind and advances beyond it.
    /// @param[in] kind Required lexical token category.
    /// @param[in] expected Diagnostic description for the grammar position.
    /// @return Success after consuming the token or a structured failure.
    [[nodiscard]] std::optional<ConfigParserError> consume(const ConfigTokenKind kind,
                                                           const ConfigParserExpectation expected)
    {
        if (current_.kind != kind)
        {
            return make_unexpected_error(expected);
        }
        return advance();
    }

    /// @brief Parses one item permitted directly inside the server block.
    /// @param[out] server Server representation receiving the parsed item.
    /// @return Success after consuming one item or a structured failure.
    [[nodiscard]] std::optional<ConfigParserError> parse_server_item(ParsedServerBlock &server)
    {
        if (current_.kind != ConfigTokenKind::identifier)
        {
            return make_unexpected_error(ConfigParserExpectation::server_item);
        }

        if (current_.lexeme == "share")
        {
            ParsedShareBlock share;
            if (auto error = parse_share(share); error.has_value())
            {
                return error;
            }
            server.shares.push_back(std::move(share));
            return std::nullopt;
        }

        const auto kind = server_directive_kind(current_.lexeme);
        if (!kind.has_value())
        {
            return make_unexpected_error(ConfigParserExpectation::server_item);
        }

        directives::ParsedServerDirective directive;
        if (auto error = parse_directive(kind.value(), expectation_for(kind.value()), directive);
            error.has_value())
        {
            return error;
        }
        server.directives.push_back(std::move(directive));
        return std::nullopt;
    }

    /// @brief Parses one complete share block and its grammatical directives.
    /// @param[out] share Owned representation receiving the parsed block.
    /// @return No value on success, otherwise a structured failure.
    [[nodiscard]] std::optional<ConfigParserError> parse_share(ParsedShareBlock &share)
    {
        share.location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return error;
        }
        if (current_.kind != ConfigTokenKind::string_literal)
        {
            return make_unexpected_error(ConfigParserExpectation::string_literal);
        }
        share.name_location = current_.location;
        share.name = current_.decoded_string.value_or(std::string{});
        if (auto error = advance(); error.has_value())
        {
            return error;
        }
        if (auto error = consume(ConfigTokenKind::left_brace, ConfigParserExpectation::left_brace);
            error.has_value())
        {
            return error;
        }

        while (current_.kind != ConfigTokenKind::right_brace &&
               current_.kind != ConfigTokenKind::end_of_input)
        {
            const auto kind = share_directive_kind(current_.lexeme);
            if (current_.kind != ConfigTokenKind::identifier || !kind.has_value())
            {
                return make_unexpected_error(ConfigParserExpectation::share_item);
            }
            directives::ParsedShareDirective directive;
            if (auto error =
                    parse_directive(kind.value(), expectation_for(kind.value()), directive);
                error.has_value())
            {
                return error;
            }
            share.directives.push_back(std::move(directive));
        }

        if (current_.kind != ConfigTokenKind::right_brace)
        {
            return make_unexpected_error(ConfigParserExpectation::right_brace);
        }
        share.closing_brace_location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return error;
        }
        return std::nullopt;
    }

    /// @brief Parses one directive with its grammar-selected scalar type and terminator.
    /// @tparam DirectiveKind Context-specific directive identity type.
    /// @tparam ParsedDirective Context-specific parsed directive type.
    /// @param[in] kind Identity of the directive name at the current token.
    /// @param[in] expected_value Scalar token category required after the name.
    /// @param[out] directive Representation receiving the parsed directive.
    /// @return No value on success, otherwise a structured failure.
    template <typename DirectiveKind, typename ParsedDirective>
    [[nodiscard]] std::optional<ConfigParserError>
    parse_directive(const DirectiveKind kind, const ConfigParserExpectation expected_value,
                    ParsedDirective &directive)
    {
        const SourceLocation directive_location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return error;
        }

        directives::ParsedConfigValue value;
        if (auto error = parse_value(expected_value, value); error.has_value())
        {
            return error;
        }

        if (auto error = consume(ConfigTokenKind::semicolon, ConfigParserExpectation::semicolon);
            error.has_value())
        {
            return error;
        }
        directive = ParsedDirective{kind, std::move(value), directive_location};
        return std::nullopt;
    }

    /// @brief Parses the scalar token type required by a directive grammar rule.
    /// @param[in] expected Required string, integer, or boolean category.
    /// @param[out] value Representation receiving the parsed scalar.
    /// @return No value on success, otherwise a structured failure.
    [[nodiscard]] std::optional<ConfigParserError>
    parse_value(const ConfigParserExpectation expected, directives::ParsedConfigValue &value)
    {
        const ConfigTokenKind required_kind = token_kind_for(expected);
        if (current_.kind != required_kind)
        {
            return make_unexpected_error(expected);
        }

        directives::ParsedConfigScalar scalar;
        if (required_kind == ConfigTokenKind::string_literal)
        {
            scalar = current_.decoded_string.value_or(std::string{});
        }
        else if (required_kind == ConfigTokenKind::boolean_literal)
        {
            scalar = current_.lexeme == "true";
        }
        else
        {
            std::uint64_t integer{};
            const char *const begin = current_.lexeme.data();
            const char *const end = begin + current_.lexeme.size();
            const auto conversion = std::from_chars(begin, end, integer);
            if (conversion.ec == std::errc::result_out_of_range || conversion.ptr != end)
            {
                return ConfigParserError{ConfigParserErrorCode::integer_out_of_range,
                                         current_.location,
                                         expected,
                                         current_.kind,
                                         std::nullopt,
                                         std::nullopt};
            }
            scalar = integer;
        }

        const SourceLocation value_location = current_.location;
        if (auto error = advance(); error.has_value())
        {
            return error;
        }
        value = directives::ParsedConfigValue{std::move(scalar), value_location};
        return std::nullopt;
    }

    /// @brief Maps a recognized server directive name to its stable identity.
    /// @param[in] name Identifier spelling from the lexer.
    /// @return Directive identity, or no value for an unknown/wrong-block name.
    [[nodiscard]] static std::optional<directives::ServerDirectiveKind>
    server_directive_kind(const std::string_view name) noexcept
    {
        if (name == "bind")
        {
            return directives::ServerDirectiveKind::bind;
        }
        if (name == "port")
        {
            return directives::ServerDirectiveKind::port;
        }
        if (name == "multithreading")
        {
            return directives::ServerDirectiveKind::multithreading;
        }
        if (name == "worker_threads")
        {
            return directives::ServerDirectiveKind::worker_threads;
        }
        if (name == "log_level")
        {
            return directives::ServerDirectiveKind::log_level;
        }
        return std::nullopt;
    }

    /// @brief Maps a recognized share directive name to its stable identity.
    /// @param[in] name Identifier spelling from the lexer.
    /// @return Directive identity, or no value for an unknown/wrong-block name.
    [[nodiscard]] static std::optional<directives::ShareDirectiveKind>
    share_directive_kind(const std::string_view name) noexcept
    {
        if (name == "path")
        {
            return directives::ShareDirectiveKind::path;
        }
        if (name == "read")
        {
            return directives::ShareDirectiveKind::read_permission;
        }
        if (name == "write")
        {
            return directives::ShareDirectiveKind::write_permission;
        }
        if (name == "delete")
        {
            return directives::ShareDirectiveKind::delete_permission;
        }
        return std::nullopt;
    }

    /// @brief Selects the scalar syntax required by one recognized directive.
    /// @param[in] kind Server directive whose value grammar is requested.
    /// @return String, integer, or boolean expectation.
    [[nodiscard]] static ConfigParserExpectation
    expectation_for(const directives::ServerDirectiveKind kind) noexcept
    {
        switch (kind)
        {
        case directives::ServerDirectiveKind::bind:
        case directives::ServerDirectiveKind::log_level:
            return ConfigParserExpectation::string_literal;
        case directives::ServerDirectiveKind::port:
        case directives::ServerDirectiveKind::worker_threads:
            return ConfigParserExpectation::integer_literal;
        case directives::ServerDirectiveKind::multithreading:
            return ConfigParserExpectation::boolean_literal;
        }
        return ConfigParserExpectation::server_item;
    }

    /// @brief Selects the scalar syntax required by one recognized share directive.
    /// @param[in] kind Share directive whose value grammar is requested.
    /// @return String or boolean expectation.
    [[nodiscard]] static ConfigParserExpectation
    expectation_for(const directives::ShareDirectiveKind kind) noexcept
    {
        switch (kind)
        {
        case directives::ShareDirectiveKind::path:
            return ConfigParserExpectation::string_literal;
        case directives::ShareDirectiveKind::read_permission:
        case directives::ShareDirectiveKind::write_permission:
        case directives::ShareDirectiveKind::delete_permission:
            return ConfigParserExpectation::boolean_literal;
        }
        return ConfigParserExpectation::share_item;
    }

    /// @brief Converts a scalar parser expectation to its lexer token category.
    /// @param[in] expected String, integer, or boolean expectation.
    /// @return Corresponding token kind.
    [[nodiscard]] static ConfigTokenKind
    token_kind_for(const ConfigParserExpectation expected) noexcept
    {
        switch (expected)
        {
        case ConfigParserExpectation::string_literal:
            return ConfigTokenKind::string_literal;
        case ConfigParserExpectation::integer_literal:
            return ConfigTokenKind::integer_literal;
        case ConfigParserExpectation::boolean_literal:
            return ConfigTokenKind::boolean_literal;
        default:
            return ConfigTokenKind::end_of_input;
        }
    }

    ConfigLexer lexer_;   ///< Token source consumed by this parser.
    ConfigToken current_; ///< Current lookahead token after initialization.
};

} // namespace

Result<ParsedConfiguration, ConfigParserError> ConfigParser::parse(ConfigLexer lexer)
{
    return ParserState(std::move(lexer)).parse();
}

const char *to_string(const ConfigParserErrorCode code) noexcept
{
    switch (code)
    {
    case ConfigParserErrorCode::lexical_error:
        return "configuration tokenization failed";
    case ConfigParserErrorCode::unexpected_token:
        return "unexpected configuration token";
    case ConfigParserErrorCode::integer_out_of_range:
        return "configuration integer is out of range";
    }
    return "unknown configuration parser error";
}

const char *to_string(const ConfigParserExpectation expectation) noexcept
{
    switch (expectation)
    {
    case ConfigParserExpectation::server_keyword:
        return "'server'";
    case ConfigParserExpectation::left_brace:
        return "'{'";
    case ConfigParserExpectation::right_brace:
        return "'}'";
    case ConfigParserExpectation::semicolon:
        return "';'";
    case ConfigParserExpectation::server_item:
        return "server directive or share block";
    case ConfigParserExpectation::share_item:
        return "share directive";
    case ConfigParserExpectation::string_literal:
        return "string literal";
    case ConfigParserExpectation::integer_literal:
        return "integer literal";
    case ConfigParserExpectation::boolean_literal:
        return "boolean literal";
    case ConfigParserExpectation::end_of_input:
        return "end of input";
    }
    return "unknown configuration syntax";
}

} // namespace sparenode::configuration
