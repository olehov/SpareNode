#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/text/utf8.hpp"

namespace
{

[[nodiscard]] std::string bytes(const std::initializer_list<unsigned int> values)
{
    std::string result;
    result.reserve(values.size());
    std::ranges::transform(values, std::back_inserter(result),
                           [](const auto value) { return static_cast<char>(value); });
    return result;
}

} // namespace

TEST_CASE("UTF-8 validation accepts canonical Unicode scalar sequences", "[foundation][utf8]")
{
    const std::array valid{
        std::string{},
        std::string{"plain ASCII"},
        std::string{"embedded\0null", 13},
        bytes({0xC2U, 0x80U}),
        bytes({0xDFU, 0xBFU}),
        bytes({0xE0U, 0xA0U, 0x80U}),
        bytes({0xEFU, 0xBFU, 0xBFU}),
        bytes({0xF0U, 0x90U, 0x80U, 0x80U}),
        bytes({0xF4U, 0x8FU, 0xBFU, 0xBFU}),
    };

    for (const auto &input : valid)
    {
        CHECK(sparenode::text::utf8::is_valid(input));
    }
}

TEST_CASE("UTF-8 validation rejects malformed and non-scalar sequences", "[foundation][utf8]")
{
    const std::array invalid{
        bytes({0x80U}),
        bytes({0xC2U}),
        bytes({0xC2U, 0x20U}),
        bytes({0xC0U, 0x80U}),
        bytes({0xE0U, 0x80U, 0x80U}),
        bytes({0xEDU, 0xA0U, 0x80U}),
        bytes({0xF0U, 0x80U, 0x80U, 0x80U}),
        bytes({0xF4U, 0x90U, 0x80U, 0x80U}),
        bytes({0xF5U, 0x80U, 0x80U, 0x80U}),
    };

    for (const auto &input : invalid)
    {
        CHECK(!sparenode::text::utf8::is_valid(input));
    }
}

TEST_CASE("UTF-8 encoding appends canonical scalar bytes", "[foundation][utf8]")
{
    std::string output;
    REQUIRE(sparenode::text::utf8::append_code_point(output, U'A'));
    REQUIRE(sparenode::text::utf8::append_code_point(output, U'\u0080'));
    REQUIRE(sparenode::text::utf8::append_code_point(output, U'\u0800'));
    REQUIRE(sparenode::text::utf8::append_code_point(output, U'\U00010000'));

    CHECK(output == std::string{"A"} +
                        bytes({0xC2U, 0x80U, 0xE0U, 0xA0U, 0x80U, 0xF0U, 0x90U, 0x80U, 0x80U}));

    const auto size = output.size();
    CHECK(!sparenode::text::utf8::append_code_point(output, char32_t{0xD800U}));
    CHECK(!sparenode::text::utf8::append_code_point(output, char32_t{0x110000U}));
    CHECK(output.size() == size);
}
