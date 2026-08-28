#pragma once

#include <iosfwd>

#include "sparenode/logging/console_log_sink.hpp"

namespace sparenode::logging::detail
{

/// @brief Detects whether one stream supports interactive terminal colors.
using ConsoleTerminalDetector = bool (*)(const std::ostream &output) noexcept;

/// @brief Resolves a console color mode using an injectable terminal detector.
///
/// Production passes its platform adapter; tests pass deterministic detectors so
/// both interactive and redirected policies are covered on Windows and Linux.
/// @param[in] output Stream inspected when automatic mode is selected.
/// @param[in] mode Requested console color policy.
/// @param[in] detector Platform adapter used only by automatic mode.
/// @return Whether the caller should emit ANSI severity colors.
[[nodiscard]] bool resolve_console_color_mode(const std::ostream &output, ConsoleColorMode mode,
                                              ConsoleTerminalDetector detector) noexcept;

} // namespace sparenode::logging::detail
