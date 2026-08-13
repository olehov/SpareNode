#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <stop_token>
#include <thread>

#include "sparenode/network/detail/accept_wait.hpp"
#include "support/fake_socket_poller.hpp"

TEST_CASE("Accept wait delegates blocking to the socket poller", "[network][tcp][cancel][unit]")
{
    using namespace std::chrono_literals;

    sparenode::test::FakeSocketPoller poller;
    const auto runtime_result = sparenode::network::detail::ensure_socket_runtime();
    REQUIRE(runtime_result.has_value());
    std::stop_source stop_source;
    using WaitResult = sparenode::Result<sparenode::network::detail::AcceptWaitStatus,
                                         sparenode::network::NetworkError>;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &stop_source, &result_promise]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_accept(
                sparenode::network::detail::invalid_socket, stop_source.get_token(), poller));
        });

    poller.wait_until_entered();
    bool stop_was_requested = false;
    std::jthread stop_thread([&stop_source, &stop_was_requested]
                             { stop_was_requested = stop_source.request_stop(); });
    while (!stop_source.stop_requested())
    {
        std::this_thread::yield();
    }
    poller.complete_with_readable(1);
    stop_thread.join();

    REQUIRE(stop_was_requested);
    REQUIRE(result_future.wait_for(2s) == std::future_status::ready);
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::AcceptWaitStatus::cancelled);
    CHECK(poller.operation() == sparenode::network::NetworkOperation::accept);
    CHECK(poller.entry_count() == 2);
}
