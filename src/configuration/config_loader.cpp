#include "sparenode/configuration/config_loader.hpp"

#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>

#include "sparenode/configuration/runtime_config_mapper.hpp"

namespace sparenode::configuration
{
namespace
{

/// @brief Reads enough bytes to preserve the lexer's configured size boundary.
/// @param[in] source_path File selected by the application user.
/// @return Complete bounded source text, or a structured file I/O failure.
[[nodiscard]] Result<std::string, ConfigFileError>
read_config_source(const std::filesystem::path &source_path)
{
    std::ifstream input(source_path, std::ios::binary);
    if (!input.is_open())
    {
        return unexpected(ConfigFileError{ConfigFileErrorCode::open_failed});
    }

    std::string source(ConfigLexer::max_input_size_bytes + 1, '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    const auto bytes_read = input.gcount();
    if (input.bad())
    {
        return unexpected(ConfigFileError{ConfigFileErrorCode::read_failed});
    }
    source.resize(static_cast<std::size_t>(bytes_read));
    return source;
}

/// @brief Appends a source-located diagnostic line.
/// @param[in,out] output Destination receiving the formatted diagnostic.
/// @param[in] path Configuration source associated with the failure.
/// @param[in] location One-based line and column of the failure.
/// @param[in] message Stable failure description.
void append_diagnostic(std::ostringstream &output, const std::filesystem::path &path,
                       const SourceLocation &location, const char *message)
{
    output << path.generic_string() << ':' << location.line << ':' << location.column
           << ": error: " << message;
}

[[nodiscard]] std::string format_failure(const std::filesystem::path &path,
                                         const ConfigFileError &failure)
{
    std::ostringstream output;
    output << "error: " << to_string(failure.code) << " '" << path.generic_string() << '\'';
    return output.str();
}

[[nodiscard]] std::string format_failure(const std::filesystem::path &path,
                                         const ConfigLexerError &failure)
{
    std::ostringstream output;
    append_diagnostic(output, path, failure.location, to_string(failure.code));
    return output.str();
}

[[nodiscard]] std::string format_failure(const std::filesystem::path &path,
                                         const ConfigParserError &failure)
{
    std::ostringstream output;
    append_diagnostic(output, path, failure.location, to_string(failure.code));
    if (failure.expected.has_value())
    {
        output << "; expected " << to_string(*failure.expected);
    }
    return output.str();
}

[[nodiscard]] std::string format_failure(const std::filesystem::path &path,
                                         const std::vector<ConfigValidationError> &failures)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < failures.size(); ++index)
    {
        append_diagnostic(output, path, failures[index].location, to_string(failures[index].code));
        if (index + 1 < failures.size())
        {
            output << '\n';
        }
    }
    return output.str();
}

} // namespace

Result<runtime::AppConfig, ConfigLoadError>
ConfigLoader::load(const std::filesystem::path &source_path)
{
    auto source_result = read_config_source(source_path);
    if (!source_result)
    {
        return unexpected(ConfigLoadError{source_path, source_result.error()});
    }

    auto lexer_result = ConfigLexer::create(source_result.value());
    if (!lexer_result)
    {
        return unexpected(ConfigLoadError{source_path, lexer_result.error()});
    }

    auto parser_result = ConfigParser::parse(std::move(lexer_result).value());
    if (!parser_result)
    {
        auto parser_error = std::move(parser_result.error());
        if (parser_error.lexer_error.has_value())
        {
            return unexpected(ConfigLoadError{source_path, std::move(*parser_error.lexer_error)});
        }
        return unexpected(ConfigLoadError{source_path, std::move(parser_error)});
    }

    auto validation_result = ConfigValidator::validate(std::move(parser_result).value());
    if (!validation_result)
    {
        return unexpected(ConfigLoadError{source_path, std::move(validation_result.error())});
    }
    return RuntimeConfigMapper::map(validation_result.value());
}

std::string format_config_load_error(const ConfigLoadError &error)
{
    return std::visit([&error](const auto &failure)
                      { return format_failure(error.source_path, failure); }, error.failure);
}

const char *to_string(const ConfigFileErrorCode code) noexcept
{
    switch (code)
    {
    case ConfigFileErrorCode::open_failed:
        return "unable to open configuration file";
    case ConfigFileErrorCode::read_failed:
        return "unable to read configuration file";
    }
    return "unknown configuration file error";
}

} // namespace sparenode::configuration
