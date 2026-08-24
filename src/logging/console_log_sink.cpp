#include "sparenode/logging/console_log_sink.hpp"

#include <ostream>

namespace sparenode::logging
{

ConsoleLogSink::ConsoleLogSink(std::ostream &output) noexcept : output_(&output)
{
}

void ConsoleLogSink::write(const LogRecord &record)
{
    const std::string formatted_record = format_log_record(record);
    std::scoped_lock lock(mutex_);
    *output_ << formatted_record << '\n';
    output_->flush();
}

} // namespace sparenode::logging
