#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "sparenode/foundation/result.hpp"

TEST_CASE("Result represents values and errors without cross-module dependencies", "[foundation]")
{
    sparenode::Result<std::string, int> success(std::string{"ready"});
    sparenode::Result<std::string, int> failure(sparenode::unexpected(7));

    REQUIRE(success.has_value());
    CHECK(success.value() == "ready");
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error() == 7);
}

TEST_CASE("Result supports move-only values and void success", "[foundation]")
{
    sparenode::Result<std::unique_ptr<int>, int> value(std::make_unique<int>(42));
    sparenode::Result<void, int> success;
    sparenode::Result<void, int> failure(sparenode::unexpected(9));

    REQUIRE(value);
    CHECK(*value.value() == 42);
    CHECK(success);
    REQUIRE_FALSE(failure);
    CHECK(failure.error() == 9);
}
