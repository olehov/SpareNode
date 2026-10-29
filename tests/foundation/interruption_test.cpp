#include <chrono>
#include <optional>
#include <stop_token>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/concurrency/interruption.hpp"

TEST_CASE("Interruption policy gives cancellation priority over expiry",
          "[foundation][concurrency]")
{
    using sparenode::concurrency::InterruptionStatus;
    const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};

    CHECK(sparenode::concurrency::interruption_status({}, std::nullopt, now) ==
          InterruptionStatus::none);
    CHECK(sparenode::concurrency::interruption_status({}, now, now) ==
          InterruptionStatus::deadline_exceeded);
    const auto future_deadline = now + std::chrono::seconds{1};
    CHECK(sparenode::concurrency::interruption_status({}, future_deadline, now) ==
          InterruptionStatus::none);

    std::stop_source stop;
    stop.request_stop();
    CHECK(sparenode::concurrency::interruption_status(stop.get_token(), now, now) ==
          InterruptionStatus::cancelled);
    CHECK(sparenode::concurrency::interruption_status(stop.get_token(), future_deadline, now) ==
          InterruptionStatus::cancelled);
}
