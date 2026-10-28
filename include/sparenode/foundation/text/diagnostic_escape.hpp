#pragma once

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace sparenode::text
{

/// @brief Selects the stable escaping contract for one diagnostic destination.
enum class DiagnosticEscapeStyle : std::uint8_t
{
    path, ///< Escape terminal control bytes as hexadecimal while preserving path spelling.
    log   ///< Also quote backslashes and use readable escapes for common whitespace controls.
};

/// @brief Appends untrusted bytes without allowing terminal or log-line control injection.
/// @param[in,out] output Destination receiving the escaped representation.
/// @param[in] value Bytes interpreted as text, never as a format string.
/// @param[in] style Destination-specific stable escaping policy.
void append_safe_diagnostic(std::ostream &output, std::string_view value,
                            DiagnosticEscapeStyle style);

} // namespace sparenode::text
