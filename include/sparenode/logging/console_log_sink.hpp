#pragma once

#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string_view>

#include "sparenode/logging/logger.hpp"

namespace sparenode::logging
{

/// @brief Selects when a console sink emits ANSI severity colors.
enum class ConsoleColorMode : std::uint8_t
{
    automatic, ///< Enable colors only when the destination is an interactive terminal.
    enabled,   ///< Always emit colors, primarily for explicit user choice and testing.
    disabled   ///< Never emit colors, even for an interactive terminal.
};

/// @brief Writes a preformatted diagnostic and colors its textual severity label.
///
/// Unlike a structured log record, this preserves compiler-style diagnostics such
/// as `file:line:column: error: message` that can occur before Logger construction.
/// The function does not append a line break.
/// @param[in,out] output Stream receiving the diagnostic.
/// @param[in] diagnostic Preformatted text whose matching severity labels are highlighted.
/// @param[in] severity Severity selecting both the textual label and terminal color.
/// @param[in] color_mode Policy controlling ANSI severity colors.
void write_console_diagnostic(std::ostream &output, std::string_view diagnostic,
                              LogSeverity severity,
                              ConsoleColorMode color_mode = ConsoleColorMode::automatic);

/// @brief Writes complete formatted records to one output stream without interleaving.
class ConsoleLogSink final : public LogSink
{
  public:
    /// @brief Creates a sink targeting the supplied terminal stream.
    /// @param[in,out] output Stream that must outlive the sink.
    /// @param[in] color_mode Policy controlling ANSI severity colors.
    explicit ConsoleLogSink(std::ostream &output,
                            ConsoleColorMode color_mode = ConsoleColorMode::automatic) noexcept;

    /// @brief Formats, writes, and flushes one complete record under a sink mutex.
    /// @param[in] record Structured record to emit.
    void write(const LogRecord &record) override;

  private:
    std::ostream *output_;  ///< Non-owning stream with lifetime guaranteed by the caller.
    bool colors_enabled_{}; ///< Whether severity markers receive ANSI terminal colors.
    std::mutex mutex_;      ///< Prevents interleaving across independent Logger instances.
};

} // namespace sparenode::logging
