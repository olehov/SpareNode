#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <future>
#include <stop_token>
#include <thread>

#include "sparenode/network/detail/socket_wait.hpp"
#include "support/fake_socket_poller.hpp"

namespace
{

using WaitResult = sparenode::Result<sparenode::network::detail::SocketWaitStatus,
                                     sparenode::network::NetworkError>;

/// Selects the terminal wake-channel state reported by the fake poller.
enum class WakeEntryCompletion : std::uint8_t
{
    socket_error,
    hangup,
    invalid,
};

[[nodiscard]] sparenode::network::detail::SocketWaitContext
wait_context(sparenode::test::FakeSocketPoller &poller,
             sparenode::network::detail::SocketWakeChannel &wake_channel)
{
    return {
        .socket = sparenode::network::detail::invalid_socket,
        .poller = poller,
        .wake_channel = wake_channel,
    };
}

/// Verifies that either readiness interest can be interrupted through the wake entry.
void check_cancellable_wait(const sparenode::network::detail::SocketWaitInterest interest,
                            const sparenode::network::NetworkOperation operation,
                            sparenode::network::detail::SocketWakeChannel &wake_channel)
{
    const auto runtime_result = sparenode::network::detail::ensure_socket_runtime();
    REQUIRE(runtime_result.has_value());
    sparenode::test::FakeSocketPoller poller;
    std::stop_source stop_source;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &stop_source, &result_promise, &wake_channel, interest, operation]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel), {.interest = interest, .operation = operation},
                stop_source.get_token()));
        });

    poller.wait_until_entered();
    REQUIRE(stop_source.request_stop());
    poller.complete_with_readable(1);

    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::cancelled);
    CHECK(poller.operation() == operation);
    CHECK(poller.entry_count() == 2);
    CHECK(wake_channel.is_initialized());
}

/// Verifies that terminal poll events remain distinguishable from normal readiness.
void check_terminal_wait(const bool report_error)
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise, &wake_channel]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel),
                {.interest = sparenode::network::detail::SocketWaitInterest::readable,
                 .operation = sparenode::network::NetworkOperation::receive}));
        });

    poller.wait_until_entered();
    if (report_error)
    {
        poller.complete_with_socket_error(0);
    }
    else
    {
        poller.complete_with_hangup(0);
    }

    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    const auto expected = report_error
                              ? sparenode::network::detail::SocketWaitStatus::socket_error
                              : sparenode::network::detail::SocketWaitStatus::socket_hangup;
    CHECK(result.value() == expected);
}

/// Verifies that an unusable wake entry produces a structured socket error.
void check_wake_entry_failure(const WakeEntryCompletion completion)
{
    const auto runtime_result = sparenode::network::detail::ensure_socket_runtime();
    REQUIRE(runtime_result.has_value());
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    std::stop_source stop_source;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel),
                {.interest = sparenode::network::detail::SocketWaitInterest::readable,
                 .operation = sparenode::network::NetworkOperation::receive},
                stop_source.get_token()));
        });

    poller.wait_until_entered();
    // A readable operation entry ensures that ignoring the wake failure would
    // incorrectly return socket_ready instead of the expected error.
    poller.stage_readable(0);
    switch (completion)
    {
    case WakeEntryCompletion::socket_error:
        poller.complete_with_socket_error(1);
        break;
    case WakeEntryCompletion::hangup:
        poller.complete_with_hangup(1);
        break;
    case WakeEntryCompletion::invalid:
        poller.complete_with_invalid(1);
        break;
    }

    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(!result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::socket);
}

} // namespace

TEST_CASE("Socket wait selects readable readiness", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise, &wake_channel]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel),
                {.interest = sparenode::network::detail::SocketWaitInterest::readable,
                 .operation = sparenode::network::NetworkOperation::receive}));
        });

    poller.wait_until_entered();
    poller.complete_with_readable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::socket_ready);
    CHECK(poller.watches_readable());
    CHECK_FALSE(poller.watches_writable());
}

TEST_CASE("Socket wait selects writable readiness", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise, &wake_channel]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel),
                {.interest = sparenode::network::detail::SocketWaitInterest::writable,
                 .operation = sparenode::network::NetworkOperation::send}));
        });

    poller.wait_until_entered();
    poller.complete_with_writable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::socket_ready);
    CHECK_FALSE(poller.watches_readable());
    CHECK(poller.watches_writable());
}

TEST_CASE("Non-stoppable socket wait does not create a wake channel", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise, &wake_channel]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                wait_context(poller, wake_channel),
                {.interest = sparenode::network::detail::SocketWaitInterest::readable,
                 .operation = sparenode::network::NetworkOperation::receive},
                std::stop_token{}));
        });

    poller.wait_until_entered();
    poller.complete_with_readable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(poller.entry_count() == 1);
    CHECK_FALSE(wake_channel.is_initialized());
}

TEST_CASE("Readable socket wait is cancellable", "[network][tcp][io][cancel][unit]")
{
    sparenode::network::detail::SocketWakeChannel wake_channel;
    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::readable,
                           sparenode::network::NetworkOperation::receive, wake_channel);
}

TEST_CASE("Writable socket wait is cancellable", "[network][tcp][io][cancel][unit]")
{
    sparenode::network::detail::SocketWakeChannel wake_channel;
    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::writable,
                           sparenode::network::NetworkOperation::send, wake_channel);
}

TEST_CASE("Socket wait distinguishes poll error readiness", "[network][tcp][io][unit][error]")
{
    check_terminal_wait(true);
}

TEST_CASE("Socket wait distinguishes poll hangup readiness", "[network][tcp][io][unit][error]")
{
    check_terminal_wait(false);
}

TEST_CASE("Cancellable socket wait rejects a wake entry error",
          "[network][tcp][io][cancel][unit][error]")
{
    check_wake_entry_failure(WakeEntryCompletion::socket_error);
}

TEST_CASE("Cancellable socket wait rejects a wake entry hangup",
          "[network][tcp][io][cancel][unit][error]")
{
    check_wake_entry_failure(WakeEntryCompletion::hangup);
}

TEST_CASE("Cancellable socket wait rejects an invalid wake entry",
          "[network][tcp][io][cancel][unit][error]")
{
    check_wake_entry_failure(WakeEntryCompletion::invalid);
}

TEST_CASE("Socket poller fake clears completion state between waits",
          "[network][tcp][io][unit][support]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    const auto context = wait_context(poller, wake_channel);
    constexpr sparenode::network::detail::SocketWaitRequest request{
        .interest = sparenode::network::detail::SocketWaitInterest::readable,
        .operation = sparenode::network::NetworkOperation::receive,
    };

    std::promise<WaitResult> first_promise;
    auto first_future = first_promise.get_future();
    std::jthread first_wait(
        [&] {
            first_promise.set_value(sparenode::network::detail::wait_for_socket(context, request));
        });
    poller.wait_until_entered();
    poller.complete_with_hangup(0);
    first_future.wait();
    const auto first_result = first_future.get();
    first_wait.join();

    std::promise<WaitResult> second_promise;
    auto second_future = second_promise.get_future();
    std::jthread second_wait(
        [&] {
            second_promise.set_value(sparenode::network::detail::wait_for_socket(context, request));
        });
    poller.wait_until_entered();
    poller.complete_with_readable(0);
    second_future.wait();
    const auto second_result = second_future.get();
    second_wait.join();

    REQUIRE(first_result.has_value());
    CHECK(first_result.value() == sparenode::network::detail::SocketWaitStatus::socket_hangup);
    REQUIRE(second_result.has_value());
    CHECK(second_result.value() == sparenode::network::detail::SocketWaitStatus::socket_ready);
}

TEST_CASE("Cancellable socket waits reuse their wake channel", "[network][tcp][io][cancel][unit]")
{
    sparenode::network::detail::SocketWakeChannel wake_channel;
    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::readable,
                           sparenode::network::NetworkOperation::receive, wake_channel);
    const auto first_reader = wake_channel.reader();

    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::writable,
                           sparenode::network::NetworkOperation::send, wake_channel);
    CHECK(wake_channel.reader() == first_reader);
}
