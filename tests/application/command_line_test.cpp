#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include "sparenode/application/command_line.hpp"

TEST_CASE("Command line requires exactly one explicit configuration path", "[application][cli]")
{
    using sparenode::application::CommandLineErrorCode;

    SECTION("valid selection")
    {
        constexpr std::array arguments{std::string_view{"--config"},
                                       std::string_view{"config/spnode.conf"}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE(result.has_value());
        CHECK(result->config_path.generic_string() == "config/spnode.conf");
    }
    SECTION("missing option")
    {
        const auto result = sparenode::application::parse_command_line({});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::missing_config);
    }
    SECTION("missing path")
    {
        constexpr std::array arguments{std::string_view{"--config"}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::missing_config_path);
    }
    SECTION("empty path")
    {
        constexpr std::array arguments{std::string_view{"--config"}, std::string_view{}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::missing_config_path);
    }
    SECTION("duplicate option")
    {
        constexpr std::array arguments{std::string_view{"--config"}, std::string_view{"one.conf"},
                                       std::string_view{"--config"}, std::string_view{"two.conf"}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::duplicate_config);
    }
    SECTION("adjacent duplicate option")
    {
        constexpr std::array arguments{std::string_view{"--config"}, std::string_view{"--config"}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::duplicate_config);
    }
    SECTION("unknown option")
    {
        constexpr std::array arguments{std::string_view{"--port"}, std::string_view{"8080"}};
        const auto result = sparenode::application::parse_command_line(arguments);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CommandLineErrorCode::unknown_argument);
    }
}
