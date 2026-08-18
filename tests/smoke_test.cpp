#include <catch2/catch_test_macros.hpp>

#include "sparenode/version.hpp"

TEST_CASE("SpareNode test infrastructure is operational", "[smoke]")
{
    REQUIRE_FALSE(sparenode::version.empty());
}
