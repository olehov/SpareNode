#include "sparenode/logging/log_severity.hpp"

namespace sparenode::logging
{

const char *to_string(const LogSeverity severity) noexcept
{
    switch (severity)
    {
    case LogSeverity::debug:
        return "DEBUG";
    case LogSeverity::info:
        return "INFO";
    case LogSeverity::warning:
        return "WARNING";
    case LogSeverity::error:
        return "ERROR";
    }

    return "UNKNOWN";
}

std::optional<LogSeverity> parse_log_severity(const std::string_view value) noexcept
{
    if (value == "debug")
    {
        return LogSeverity::debug;
    }
    if (value == "info")
    {
        return LogSeverity::info;
    }
    if (value == "warning")
    {
        return LogSeverity::warning;
    }
    if (value == "error")
    {
        return LogSeverity::error;
    }
    return std::nullopt;
}

} // namespace sparenode::logging
