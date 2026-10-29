#include <array>
#include <cstdint>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/text/numeric_ip_address.hpp"
#include "support/optional.hpp"

TEST_CASE("Numeric IP parser accepts canonical IPv4 and complete IPv6 forms", "[foundation][ip]")
{
    using namespace sparenode::text::ip;
    const std::array valid_ipv4{std::string_view{"127.0.0.1"}, std::string_view{"0.0.0.0"},
                                std::string_view{"255.255.255.255"}};
    for (const auto value : valid_ipv4)
    {
        INFO(value);
        CHECK(is_valid_ipv4_address(value));
    }

    const std::array valid_ipv6{std::string_view{"::1"},
                                std::string_view{"::"},
                                std::string_view{"2001:db8::1"},
                                std::string_view{"::ffff:192.0.2.1"},
                                std::string_view{"2001:db8:0:0:0:0:2:1"},
                                std::string_view{"1:2:3:4:5:6:7::"}};
    for (const auto value : valid_ipv6)
    {
        INFO(value);
        CHECK(is_valid_ipv6_address(value));
    }
}

TEST_CASE("Numeric IP parser rejects ambiguous or malformed forms", "[foundation][ip]")
{
    using sparenode::text::ip::parse_numeric_address;
    const std::array invalid{
        std::string_view{"127.1"},
        std::string_view{"01.2.3.4"},
        std::string_view{"256.0.0.1"},
        std::string_view{"1::2::3"},
        std::string_view{"2001:db8"},
        std::string_view{"::ffff:01.2.3.4"},
        std::string_view{"[::1]"},
        std::string_view{"::1%1"},
        std::string_view{""},
        std::string_view{":::"},
        std::string_view{"1::2:"},
        std::string_view{":1::"},
        std::string_view{"1.2.3.4::"},
        std::string_view{"::1.2.3.4:5"},
        std::string_view{"1:2:3:4:5:6:7:1.2.3.4"},
        std::string_view{"1:2:3:4:5:6:7:8:9"},
        std::string_view{"12345::"},
    };
    for (const auto value : invalid)
    {
        INFO(value);
        CHECK(!parse_numeric_address(value).has_value());
    }
}

TEST_CASE("Numeric IP classification uses parsed network-order bytes", "[foundation][ip]")
{
    using namespace sparenode::text::ip;
    const auto ipv4_loopback = parse_numeric_address("127.42.0.1");
    const auto ipv6_loopback = parse_numeric_address("::1");
    const auto ipv4_any = parse_numeric_address("0.0.0.0");
    const auto ipv6_any = parse_numeric_address("::");

    CHECK(is_loopback_address(sparenode::test::require_optional(ipv4_loopback)));
    CHECK(is_loopback_address(sparenode::test::require_optional(ipv6_loopback)));
    CHECK(is_unspecified_address(sparenode::test::require_optional(ipv4_any)));
    CHECK(is_unspecified_address(sparenode::test::require_optional(ipv6_any)));

    CHECK(!is_loopback_address(
        sparenode::test::require_optional(parse_numeric_address("128.0.0.1"))));
    CHECK(!is_loopback_address(sparenode::test::require_optional(parse_numeric_address("::2"))));
    CHECK(!is_loopback_address(
        sparenode::test::require_optional(parse_numeric_address("::ffff:127.0.0.1"))));
    CHECK(!is_unspecified_address(
        sparenode::test::require_optional(parse_numeric_address("0.0.0.1"))));
    CHECK(is_unspecified_address(
        sparenode::test::require_optional(parse_numeric_address("::ffff:0.0.0.0"))));
}

TEST_CASE("Numeric IP parser stores IPv6 words in network byte order", "[foundation][ip]")
{
    using sparenode::text::ip::parse_numeric_address;
    const auto mapped =
        sparenode::test::require_optional(parse_numeric_address("::ffff:192.0.2.1"));
    const std::array<std::uint8_t, 16> expected_mapped{0, 0, 0,     0,     0,   0, 0, 0,
                                                       0, 0, 0xFFU, 0xFFU, 192, 0, 2, 1};
    CHECK(mapped.bytes == expected_mapped);

    const auto documentation =
        sparenode::test::require_optional(parse_numeric_address("2001:db8::1"));
    const std::array<std::uint8_t, 16> expected_documentation{
        0x20U, 0x01U, 0x0DU, 0xB8U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(documentation.bytes == expected_documentation);
}
