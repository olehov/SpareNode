#include "sparenode/logging/logger.hpp"

#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sparenode::logging
{

namespace
{

/// @brief Appends untrusted text without allowing it to create additional log lines.
/// @param[in,out] output Destination receiving escaped text.
/// @param[in] value Text interpreted only as bytes, never as a format string.
void append_escaped(std::ostream &output, const std::string_view value)
{
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20 || character == 0x7F)
            {
                output << "\\x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
}

/// @brief Converts a timestamp to UTC using the platform's thread-safe API.
/// @param[in] timestamp Calendar time to convert.
/// @return Broken-down UTC time.
[[nodiscard]] std::tm to_utc(const std::time_t timestamp)
{
    std::tm utc{};
#ifdef _WIN32
    if (::gmtime_s(&utc, &timestamp) != 0)
    {
        throw std::runtime_error("failed to convert log timestamp");
    }
#else
    if (::gmtime_r(&timestamp, &utc) == nullptr)
    {
        throw std::runtime_error("failed to convert log timestamp");
    }
#endif
    return utc;
}

} // namespace

/// @brief Holds synchronization state shared by every Logger copy.
struct Logger::State final
{
    /// @brief Stores an immutable sink and filtering threshold.
    /// @param[in] destination Destination receiving enabled records.
    /// @param[in] threshold Lowest enabled severity.
    State(std::shared_ptr<LogSink> destination, const LogSeverity threshold) noexcept
        : sink(std::move(destination)), minimum_severity(threshold)
    {
    }

    std::shared_ptr<LogSink> sink; ///< Injected destination, possibly empty.
    LogSeverity minimum_severity;  ///< Immutable filtering threshold.
    std::mutex mutex;              ///< Serializes calls into any injected sink.
};

Logger::Logger(std::shared_ptr<LogSink> sink, const LogSeverity minimum_severity)
    : state_(std::make_shared<State>(std::move(sink), minimum_severity))
{
}

void Logger::log(const LogEvent &event) const noexcept
{
    if (!is_enabled(event.severity))
    {
        return;
    }

    try
    {
        LogRecord record{std::chrono::system_clock::now(), event.severity,
                         std::string(event.subsystem), std::string(event.message)};
        std::scoped_lock lock(state_->mutex);
        state_->sink->write(record);
    }
    catch (...)
    {
        // Logging is diagnostic-only and must never alter application control flow.
        return;
    }
}

bool Logger::is_enabled(const LogSeverity severity) const noexcept
{
    return state_ && state_->sink && severity >= state_->minimum_severity;
}

LogSeverity Logger::minimum_severity() const noexcept
{
    return state_ ? state_->minimum_severity : LogSeverity::error;
}

std::string format_log_record(const LogRecord &record)
{
    const auto seconds = std::chrono::floor<std::chrono::seconds>(record.timestamp);
    const auto fractional_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp - seconds);
    const std::time_t calendar_time = std::chrono::system_clock::to_time_t(seconds);
    const std::tm utc = to_utc(calendar_time);

    std::ostringstream output;
    output << '[' << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << fractional_milliseconds.count() << "Z] ["
           << to_string(record.severity) << "] [";
    append_escaped(output, record.subsystem);
    output << "] ";
    append_escaped(output, record.message);
    return output.str();
}

} // namespace sparenode::logging
