#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "sparenode/logging/log_severity.hpp"

namespace sparenode::logging
{

/// @brief Stores the complete structured data associated with one diagnostic event.
struct LogRecord
{
    std::chrono::system_clock::time_point timestamp; ///< Time at which logging was requested.
    LogSeverity severity{};                          ///< Operational importance of the record.
    std::string subsystem;                           ///< Stable component or category name.
    std::string message;                             ///< Human-readable text without secrets.
};

/// @brief Groups caller-provided fields before the logger adds a timestamp.
struct LogEvent
{
    LogSeverity severity{};     ///< Operational importance of the event.
    std::string_view subsystem; ///< Stable component or category name.
    std::string_view message;   ///< Human-readable text treated only as data.
};

/// @brief Receives structured records without imposing a concrete output backend.
class LogSink
{
  public:
    virtual ~LogSink() = default;

    /// @brief Emits one complete record.
    /// @param[in] record Structured record valid for the duration of the call.
    virtual void write(const LogRecord &record) = 0;

  protected:
    /// @brief Creates the base portion of a concrete sink.
    LogSink() = default;

    /// @brief Copies base sink state for a concrete sink that supports copying.
    /// @param[in] other Base state to copy.
    LogSink(const LogSink &other) = default;

    /// @brief Moves base sink state for a concrete sink that supports moving.
    /// @param[in,out] other Base state to move.
    LogSink(LogSink &&other) noexcept = default;

    /// @brief Copies base sink state during derived assignment.
    /// @param[in] other Base state to copy.
    /// @return This base object after assignment.
    LogSink &operator=(const LogSink &other) = default;

    /// @brief Moves base sink state during derived assignment.
    /// @param[in,out] other Base state to move.
    /// @return This base object after assignment.
    LogSink &operator=(LogSink &&other) noexcept = default;
};

/// @brief Filters and serializes records before forwarding them to an injected sink.
///
/// Copies share the same sink and synchronization state, which makes captures in
/// asynchronous observers safe. Every logging call contains allocation, sink, and
/// output failures so diagnostics cannot escape application or worker boundaries.
class Logger final
{
  public:
    /// @brief Creates a logger for one sink and minimum emitted severity.
    /// @param[in] sink Destination shared by every copy of this logger.
    /// @param[in] minimum_severity Lowest severity forwarded to the sink.
    explicit Logger(std::shared_ptr<LogSink> sink,
                    LogSeverity minimum_severity = LogSeverity::info);

    /// @brief Emits a record when its severity passes the configured threshold.
    /// @param[in] event Severity, subsystem, and message treated only as data.
    void log(const LogEvent &event) const noexcept;

    /// @brief Reports whether a severity would be forwarded to the sink.
    /// @param[in] severity Severity to compare with the configured threshold.
    /// @return `true` when a call at this severity is enabled.
    [[nodiscard]] bool is_enabled(LogSeverity severity) const noexcept;

    /// @brief Returns the immutable minimum severity.
    /// @return Lowest severity forwarded by this logger.
    [[nodiscard]] LogSeverity minimum_severity() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_; ///< Shared sink, threshold, and serialization boundary.
};

/// @brief Formats one record as a single UTC console line.
/// @param[in] record Structured data to format without modifying it.
/// @return Timestamped line with escaped subsystem and message text.
[[nodiscard]] std::string format_log_record(const LogRecord &record);

} // namespace sparenode::logging
