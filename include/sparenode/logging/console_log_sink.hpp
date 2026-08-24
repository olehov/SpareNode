#pragma once

#include <iosfwd>
#include <mutex>

#include "sparenode/logging/logger.hpp"

namespace sparenode::logging
{

/// @brief Writes complete formatted records to one output stream without interleaving.
class ConsoleLogSink final : public LogSink
{
  public:
    /// @brief Creates a sink targeting the supplied terminal stream.
    /// @param[in,out] output Stream that must outlive the sink.
    explicit ConsoleLogSink(std::ostream &output) noexcept;

    /// @brief Formats, writes, and flushes one complete record under a sink mutex.
    /// @param[in] record Structured record to emit.
    void write(const LogRecord &record) override;

  private:
    std::ostream *output_; ///< Non-owning stream with lifetime guaranteed by the caller.
    std::mutex mutex_;     ///< Prevents interleaving across independent Logger instances.
};

} // namespace sparenode::logging
