#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/detail/console_color_detection.hpp"
#include "sparenode/logging/logger.hpp"
#include "sparenode/logging/network_logging.hpp"
#include "sparenode/network/network_error.hpp"

namespace
{

class CapturingSink final : public sparenode::logging::LogSink
{
  public:
    void write(const sparenode::logging::LogRecord &record) override
    {
        records_.push_back(record);
    }

    [[nodiscard]] const std::vector<sparenode::logging::LogRecord> &records() const noexcept
    {
        return records_;
    }

  private:
    std::vector<sparenode::logging::LogRecord> records_;
};

class FailingSink final : public sparenode::logging::LogSink
{
  public:
    void write(const sparenode::logging::LogRecord &) override
    {
        throw std::runtime_error("deliberate sink failure");
    }
};

/// @brief Deterministically represents an interactive terminal in color-policy tests.
[[nodiscard]] bool reports_interactive_terminal(const std::ostream &) noexcept
{
    return true;
}

/// @brief Deterministically represents a redirected stream in color-policy tests.
[[nodiscard]] bool reports_redirected_stream(const std::ostream &) noexcept
{
    return false;
}

} // namespace

TEST_CASE("Log formatting preserves structure and escapes line injection", "[logging][format]")
{
    const sparenode::logging::LogRecord record{
        std::chrono::system_clock::time_point{std::chrono::milliseconds{123}},
        sparenode::logging::LogSeverity::warning, "network\naccept", "peer said %s\r\nnext"};

    CHECK(sparenode::logging::format_log_record(record) ==
          "[1970-01-01T00:00:00.123Z] [WARNING] [network\\naccept] peer said %s\\r\\nnext");
}

TEST_CASE("Console logging emits and flushes one complete line", "[logging][console]")
{
    std::ostringstream output;
    sparenode::logging::ConsoleLogSink sink(output, sparenode::logging::ConsoleColorMode::disabled);
    const sparenode::logging::LogRecord record{std::chrono::system_clock::time_point{},
                                               sparenode::logging::LogSeverity::info, "application",
                                               "started"};

    sink.write(record);

    CHECK(output.str() == "[1970-01-01T00:00:00.000Z] [INFO] [application] started\n");
}

TEST_CASE("Console logging colors only the severity marker", "[logging][console][color]")
{
    std::ostringstream output;
    sparenode::logging::ConsoleLogSink sink(output, sparenode::logging::ConsoleColorMode::enabled);
    const sparenode::logging::LogRecord record{std::chrono::system_clock::time_point{},
                                               sparenode::logging::LogSeverity::error,
                                               "application", "startup failed"};

    sink.write(record);

    CHECK(output.str() ==
          "[1970-01-01T00:00:00.000Z] \x1b[31m[ERROR]\x1b[0m [application] startup failed\n");
}

TEST_CASE("Automatic console coloring keeps redirected output plain", "[logging][console][color]")
{
    std::ostringstream output;
    sparenode::logging::ConsoleLogSink sink(output);
    const sparenode::logging::LogRecord record{std::chrono::system_clock::time_point{},
                                               sparenode::logging::LogSeverity::error,
                                               "application", "startup failed"};

    sink.write(record);

    CHECK(output.str() == "[1970-01-01T00:00:00.000Z] [ERROR] [application] startup failed\n");
}

TEST_CASE("Automatic console color policy follows platform terminal detection",
          "[logging][console][color]")
{
    using sparenode::logging::ConsoleColorMode;
    using sparenode::logging::detail::resolve_console_color_mode;
    std::ostringstream output;

    CHECK(resolve_console_color_mode(output, ConsoleColorMode::automatic,
                                     reports_interactive_terminal));
    CHECK_FALSE(
        resolve_console_color_mode(output, ConsoleColorMode::automatic, reports_redirected_stream));
    CHECK(resolve_console_color_mode(output, ConsoleColorMode::enabled, reports_redirected_stream));
    CHECK_FALSE(resolve_console_color_mode(output, ConsoleColorMode::disabled,
                                           reports_interactive_terminal));
}

TEST_CASE("Console diagnostics color every error label without changing their layout",
          "[logging][console][color]")
{
    std::ostringstream output;

    sparenode::logging::write_console_diagnostic(
        output, "config.conf:2:4: error: first\nconfig.conf:3:8: error: second",
        sparenode::logging::LogSeverity::error, sparenode::logging::ConsoleColorMode::enabled);

    CHECK(output.str() == "config.conf:2:4: \x1b[31merror:\x1b[0m first\n"
                          "config.conf:3:8: \x1b[31merror:\x1b[0m second");
}

TEST_CASE("Console sink serializes independent logger instances", "[logging][console][concurrency]")
{
    constexpr std::size_t records_per_logger = 100;
    std::ostringstream output;
    auto sink = std::make_shared<sparenode::logging::ConsoleLogSink>(output);
    const sparenode::logging::Logger first_logger(sink);
    const sparenode::logging::Logger second_logger(sink);

    std::thread first_worker(
        [first_logger]
        {
            for (std::size_t index = 0; index < records_per_logger; ++index)
            {
                first_logger.log({sparenode::logging::LogSeverity::info, "worker", "first"});
            }
        });
    std::thread second_worker(
        [second_logger]
        {
            for (std::size_t index = 0; index < records_per_logger; ++index)
            {
                second_logger.log({sparenode::logging::LogSeverity::info, "worker", "second"});
            }
        });
    first_worker.join();
    second_worker.join();

    std::istringstream emitted_lines(output.str());
    std::string line;
    std::size_t line_count = 0;
    while (std::getline(emitted_lines, line))
    {
        constexpr std::string_view record_marker = "] [INFO] [worker] ";
        CHECK(line.starts_with('['));
        CHECK(line.find(record_marker) == 25);
        CHECK(line.find(record_marker, 26) == std::string::npos);
        CHECK((line.ends_with("] [INFO] [worker] first") ||
               line.ends_with("] [INFO] [worker] second")));
        ++line_count;
    }
    CHECK(line_count == records_per_logger * 2);
}

TEST_CASE("Log severity parser accepts only documented configuration values",
          "[logging][configuration]")
{
    CHECK(sparenode::logging::parse_log_severity("debug") ==
          sparenode::logging::LogSeverity::debug);
    CHECK(sparenode::logging::parse_log_severity("info") == sparenode::logging::LogSeverity::info);
    CHECK(sparenode::logging::parse_log_severity("warning") ==
          sparenode::logging::LogSeverity::warning);
    CHECK(sparenode::logging::parse_log_severity("error") ==
          sparenode::logging::LogSeverity::error);
    CHECK_FALSE(sparenode::logging::parse_log_severity("").has_value());
    CHECK_FALSE(sparenode::logging::parse_log_severity("INFO").has_value());
}

TEST_CASE("Logger filters records below its configured severity", "[logging][filter]")
{
    auto sink = std::make_shared<CapturingSink>();
    const sparenode::logging::Logger logger(sink, sparenode::logging::LogSeverity::warning);

    logger.log({sparenode::logging::LogSeverity::debug, "test", "debug"});
    logger.log({sparenode::logging::LogSeverity::info, "test", "info"});
    logger.log({sparenode::logging::LogSeverity::warning, "test", "warning"});
    logger.log({sparenode::logging::LogSeverity::error, "test", "error"});

    REQUIRE(sink->records().size() == 2);
    CHECK(sink->records()[0].severity == sparenode::logging::LogSeverity::warning);
    CHECK(sink->records()[1].severity == sparenode::logging::LogSeverity::error);
}

TEST_CASE("Logger serializes concurrent writes into an unsynchronized sink",
          "[logging][concurrency]")
{
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t records_per_thread = 100;
    auto sink = std::make_shared<CapturingSink>();
    const sparenode::logging::Logger logger(sink, sparenode::logging::LogSeverity::debug);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t thread = 0; thread < thread_count; ++thread)
    {
        workers.emplace_back(
            [logger]
            {
                for (std::size_t record = 0; record < records_per_thread; ++record)
                {
                    logger.log(
                        {sparenode::logging::LogSeverity::info, "worker", "complete record"});
                }
            });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    REQUIRE(sink->records().size() == thread_count * records_per_thread);
    for (const auto &record : sink->records())
    {
        CHECK(record.subsystem == "worker");
        CHECK(record.message == "complete record");
    }
}

TEST_CASE("Logger contains exceptions raised by its sink", "[logging][failure]")
{
    const sparenode::logging::Logger logger(std::make_shared<FailingSink>());

    CHECK_NOTHROW(logger.log({sparenode::logging::LogSeverity::error, "test", "cannot escape"}));
}

TEST_CASE("Network logging preserves structured operation domain and code", "[logging][network]")
{
    const sparenode::network::NetworkError error{sparenode::network::NetworkOperation::receive,
                                                 sparenode::network::NetworkErrorDomain::socket,
                                                 42};

    CHECK(sparenode::logging::format_network_error(error) ==
          "operation=receive domain=socket code=42");
}

TEST_CASE("Network observers report worker and server failure boundaries", "[logging][network]")
{
    auto sink = std::make_shared<CapturingSink>();
    const sparenode::logging::Logger logger(sink);
    auto connection_observer = sparenode::logging::make_connection_failure_log_observer(logger);
    auto server_observer = sparenode::logging::make_connection_server_failure_log_observer(logger);

    connection_observer({sparenode::network::ConnectionFailureKind::handler_error,
                         sparenode::network::NetworkError{
                             sparenode::network::NetworkOperation::send,
                             sparenode::network::NetworkErrorDomain::cancellation, 0}});
    server_observer(
        {sparenode::network::ConnectionServerFailureKind::dispatch_error, std::nullopt,
         sparenode::network::DispatchError{sparenode::network::DispatchErrorCode::stopped, 7}});

    REQUIRE(sink->records().size() == 2);
    CHECK(sink->records()[0].message == "handler_error operation=send domain=cancellation code=0");
    CHECK(sink->records()[1].message == "dispatch_error dispatch_code=stopped native_code=7");
}
