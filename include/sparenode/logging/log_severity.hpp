#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace sparenode::logging
{

/// @brief Identifies the operational importance of one log record.
enum class LogSeverity : std::uint8_t
{
    debug,   ///< Detailed diagnostics intended for development and investigation.
    info,    ///< Normal application lifecycle and configuration events.
    warning, ///< Recoverable conditions that may require operator attention.
    error    ///< Failures that prevent or terminate an operation.
};

/// @brief Returns the stable uppercase name of a log severity.
/// @param[in] severity Severity to describe.
/// @return Static English name suitable for formatted log records.
[[nodiscard]] const char *to_string(LogSeverity severity) noexcept;

/// @brief Parses a case-sensitive configuration value into a severity.
/// @param[in] value Untrusted configuration text to inspect without modifying it.
/// @return Parsed severity, or no value when the text is unsupported.
[[nodiscard]] std::optional<LogSeverity> parse_log_severity(std::string_view value) noexcept;

} // namespace sparenode::logging
