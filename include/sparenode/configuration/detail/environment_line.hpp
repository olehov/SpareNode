#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace sparenode::configuration::detail
{

/// @brief Stores one parsed key-value assignment from an environment file.
struct EnvironmentAssignment
{
    std::string key;   ///< Environment variable name before the equals sign.
    std::string value; ///< Trimmed and optionally unquoted variable value.
};

/// @brief Describes an environment-file line that cannot be parsed safely.
struct MalformedEnvironmentLine
{
    std::string variable; ///< Variable name when one could be identified.
};

/// @brief Represents an ignored line, a parsed assignment, or a malformed line.
using ParsedEnvironmentLine =
    std::variant<std::monostate, EnvironmentAssignment, MalformedEnvironmentLine>;

/// @brief Parses one line from a UTF-8 environment file.
/// @param[in] line Raw line content without the newline delimiter.
/// @param[in] line_number One-based source line used to handle an initial UTF-8 BOM.
/// @return An ignored-line marker, parsed assignment, or malformed-line description.
[[nodiscard]] ParsedEnvironmentLine parse_environment_line(std::string_view line,
                                                           std::size_t line_number);

} // namespace sparenode::configuration::detail
