#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "sparenode/http/detail/request_deadlines.hpp"
#include "support/optional.hpp"

TEST_CASE("HTTP inactivity deadlines renew only on progress and retain the total cap",
          "[http][timeout][policy]")
{
    const sparenode::network::NetworkDeadline started{std::chrono::seconds{100}};
    auto state =
        sparenode::http::detail::RequestDeadlines::create({.headers = std::chrono::seconds{5},
                                                           .body = std::chrono::seconds{8},
                                                           .total = std::chrono::seconds{12}},
                                                          started);
    auto &deadlines = sparenode::test::require_optional(state);

    CHECK(deadlines.next(false) == started + std::chrono::seconds{5});
    // Recomputing receive options alone must not renew inactivity.
    CHECK(deadlines.next(false) == started + std::chrono::seconds{5});
    deadlines.record_progress(started + std::chrono::seconds{4});
    CHECK(deadlines.next(false) == started + std::chrono::seconds{9});
    CHECK(deadlines.next(true) == started + std::chrono::seconds{12});
    deadlines.record_progress(started + std::chrono::seconds{8});
    CHECK(deadlines.next(false) == deadlines.total());
    CHECK(deadlines.next(true) == deadlines.total());
    CHECK(deadlines.total() == started + std::chrono::seconds{12});
}

TEST_CASE("HTTP deadline policy validates exact representable boundaries without a live clock",
          "[http][timeout][limits]")
{
    const auto maximum = sparenode::network::NetworkDeadline::max();
    const auto started = maximum - std::chrono::seconds{20};
    const sparenode::http::HttpRequestTimeouts accepted{.headers = std::chrono::seconds{20},
                                                        .body = std::chrono::seconds{20},
                                                        .total = std::chrono::seconds{20}};
    auto state = sparenode::http::detail::RequestDeadlines::create(accepted, started);
    auto &deadlines = sparenode::test::require_optional(state);
    CHECK(deadlines.total() == maximum);
    deadlines.record_progress(started + std::chrono::seconds{1});
    CHECK(deadlines.next(false) == maximum);
    CHECK(deadlines.next(true) == maximum);

    auto rejected = accepted;
    rejected.total += std::chrono::milliseconds{1};
    CHECK_FALSE(sparenode::http::detail::RequestDeadlines::create(rejected, started).has_value());
    rejected = accepted;
    rejected.headers = std::chrono::milliseconds{0};
    CHECK_FALSE(sparenode::http::detail::RequestDeadlines::create(rejected, started).has_value());
    rejected = accepted;
    rejected.body = std::chrono::milliseconds{-1};
    CHECK_FALSE(sparenode::http::detail::RequestDeadlines::create(rejected, started).has_value());
    rejected = accepted;
    rejected.headers = std::chrono::milliseconds::max();
    CHECK_FALSE(sparenode::http::detail::RequestDeadlines::create(rejected, started).has_value());
}
