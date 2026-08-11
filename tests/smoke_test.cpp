#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("SpareNode test infrastructure is operational", "[smoke]")
{
    constexpr std::string_view project_name{"SpareNode"};

    REQUIRE(project_name == "SpareNode");
}
