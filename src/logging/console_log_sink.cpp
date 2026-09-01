#include "sparenode/logging/console_log_sink.hpp"

#include "sparenode/logging/detail/console_color_detection.hpp"

#include <cstdio>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sparenode::logging
{

namespace
{

constexpr std::string_view ansi_reset = "\x1b[0m";
constexpr std::string_view ansi_cyan = "\x1b[36m";
constexpr std::string_view ansi_green = "\x1b[32m";
constexpr std::string_view ansi_yellow = "\x1b[33m";
constexpr std::string_view ansi_red = "\x1b[31m";

/// @brief Maps each severity to a conventional terminal color.
/// @param[in] severity Severity whose marker will be rendered.
/// @return ANSI Select Graphic Rendition sequence for the severity.
[[nodiscard]] constexpr std::string_view severity_color(const LogSeverity severity) noexcept
{
    switch (severity)
    {
    case LogSeverity::debug:
        return ansi_cyan;
    case LogSeverity::info:
        return ansi_green;
    case LogSeverity::warning:
        return ansi_yellow;
    case LogSeverity::error:
        return ansi_red;
    }
    return {};
}

/// @brief Maps each severity to the lowercase label used by startup diagnostics.
/// @param[in] severity Severity whose textual label is required.
/// @return Diagnostic label including its trailing colon.
[[nodiscard]] constexpr std::string_view diagnostic_label(const LogSeverity severity) noexcept
{
    switch (severity)
    {
    case LogSeverity::debug:
        return "debug:";
    case LogSeverity::info:
        return "info:";
    case LogSeverity::warning:
        return "warning:";
    case LogSeverity::error:
        return "error:";
    }
    return {};
}

/// @brief Identifies the C file behind a known standard C++ stream.
/// @param[in] output Candidate standard output stream.
/// @return `stdout`, `stderr`, or `nullptr` for a non-standard stream.
[[nodiscard]] std::FILE *standard_file_for(const std::ostream &output) noexcept
{
    if (&output == &std::cout)
    {
        return stdout;
    }
    if (&output == &std::cerr || &output == &std::clog)
    {
        return stderr;
    }
    return nullptr;
}

#ifdef _WIN32
/// @brief Enables ANSI processing for an interactive Windows console stream.
/// @param[in] output Candidate standard terminal stream.
/// @return `true` when the stream is a console that accepts ANSI colors.
[[nodiscard]] bool enable_terminal_colors(const std::ostream &output) noexcept
{
    std::FILE *const file = standard_file_for(output);
    if (file == nullptr || ::_isatty(::_fileno(file)) == 0)
    {
        return false;
    }

    const DWORD standard_handle = file == stdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
    const HANDLE handle = ::GetStdHandle(standard_handle);
    DWORD mode = 0;
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !::GetConsoleMode(handle, &mode))
    {
        return false;
    }
    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#else
/// @brief Detects whether a standard POSIX stream is attached to a terminal.
/// @param[in] output Candidate standard terminal stream.
/// @return `true` when ANSI colors can be emitted interactively.
[[nodiscard]] bool enable_terminal_colors(const std::ostream &output) noexcept
{
    std::FILE *const file = standard_file_for(output);
    return file != nullptr && ::isatty(::fileno(file)) != 0;
}
#endif

/// @brief Inserts ANSI color around the severity marker in a formatted record.
/// @param[in] formatted_record Plain formatted log line.
/// @param[in] severity Severity selecting the marker and color.
/// @return Colored line, or the original line if its expected marker is absent.
[[nodiscard]] std::string colorize_severity(std::string formatted_record,
                                            const LogSeverity severity)
{
    const std::string_view color = severity_color(severity);
    if (color.empty())
    {
        return formatted_record;
    }

    const std::string marker = '[' + std::string(to_string(severity)) + ']';
    const std::size_t marker_position = formatted_record.find(marker);
    if (marker_position == std::string::npos)
    {
        return formatted_record;
    }

    formatted_record.insert(marker_position + marker.size(), ansi_reset.data(), ansi_reset.size());
    formatted_record.insert(marker_position, color.data(), color.size());
    return formatted_record;
}

/// @brief Colors every matching textual severity label without changing other text.
/// @param[in] diagnostic Preformatted diagnostic text.
/// @param[in] severity Severity selecting the label and color.
/// @return Diagnostic with ANSI sequences surrounding each matching label.
[[nodiscard]] std::string colorize_diagnostic(std::string diagnostic, const LogSeverity severity)
{
    const std::string_view label = diagnostic_label(severity);
    const std::string_view color = severity_color(severity);
    if (label.empty() || color.empty())
    {
        return diagnostic;
    }

    std::size_t search_position = 0;
    while ((search_position = diagnostic.find(label, search_position)) != std::string::npos)
    {
        diagnostic.insert(search_position + label.size(), ansi_reset.data(), ansi_reset.size());
        diagnostic.insert(search_position, color.data(), color.size());
        search_position += color.size() + label.size() + ansi_reset.size();
    }
    return diagnostic;
}

} // namespace

bool detail::resolve_console_color_mode(const std::ostream &output, const ConsoleColorMode mode,
                                        const ConsoleTerminalDetector detector) noexcept
{
    switch (mode)
    {
    case ConsoleColorMode::automatic:
        return detector != nullptr && detector(output);
    case ConsoleColorMode::enabled:
        if (detector != nullptr)
        {
            static_cast<void>(detector(output));
        }
        return true;
    case ConsoleColorMode::disabled:
        return false;
    }
    return false;
}

void write_console_diagnostic(std::ostream &output, const std::string_view diagnostic,
                              const LogSeverity severity, const ConsoleColorMode color_mode)
{
    if (detail::resolve_console_color_mode(output, color_mode, enable_terminal_colors))
    {
        output << colorize_diagnostic(std::string(diagnostic), severity);
    }
    else
    {
        output << diagnostic;
    }
    output.flush();
}

ConsoleLogSink::ConsoleLogSink(std::ostream &output, const ConsoleColorMode color_mode) noexcept
    : output_(&output), colors_enabled_(detail::resolve_console_color_mode(output, color_mode,
                                                                           enable_terminal_colors))
{
}

void ConsoleLogSink::write(const LogRecord &record)
{
    std::string formatted_record = format_log_record(record);
    if (colors_enabled_)
    {
        formatted_record = colorize_severity(std::move(formatted_record), record.severity);
    }
    std::scoped_lock lock(mutex_);
    *output_ << formatted_record << '\n';
    output_->flush();
}

} // namespace sparenode::logging
