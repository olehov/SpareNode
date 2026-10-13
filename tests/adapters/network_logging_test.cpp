#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

#include "sparenode/adapters/network_logging.hpp"
#include "sparenode/logging/logger.hpp"
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

} // namespace

TEST_CASE("Network logging preserves structured operation domain and code",
          "[adapter][logging][network]")
{
    const sparenode::network::NetworkError error{sparenode::network::NetworkOperation::receive,
                                                 sparenode::network::NetworkErrorDomain::socket,
                                                 42};

    CHECK(sparenode::logging::format_network_error(error) ==
          "operation=receive domain=socket code=42");

    const sparenode::network::NetworkError timeout{sparenode::network::NetworkOperation::receive,
                                                   sparenode::network::NetworkErrorDomain::timeout,
                                                   0};
    CHECK(sparenode::logging::format_network_error(timeout) ==
          "operation=receive domain=timeout code=0");
}

TEST_CASE("Network observers report worker and server failure boundaries",
          "[adapter][logging][network]")
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
