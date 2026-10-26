#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

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
        bytes({0xEDU, 0x9FU, 0xBFU}),
        bytes({0xEEU, 0x80U, 0x80U}),
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
        bytes({0xC1U, 0xBFU}),
        bytes({0xE0U, 0x80U, 0x80U}),
        bytes({0xE2U, 0x82U}),
        bytes({0xE2U, 0x82U, 0x20U}),
        bytes({0xEDU, 0xA0U, 0x80U}),
        bytes({0xEDU, 0xBFU, 0xBFU}),
        bytes({0xF0U, 0x80U, 0x80U, 0x80U}),
        bytes({0xF0U, 0x90U, 0x80U}),
        bytes({0xF0U, 0x90U, 0x80U, 0x20U}),
        bytes({0xF4U, 0x90U, 0x80U, 0x80U}),
        bytes({0xF5U, 0x80U, 0x80U, 0x80U}),
        bytes({0xFFU}),
    };

    for (const auto &input : invalid)
    {
        CHECK(!sparenode::text::utf8::is_valid(input));
    }
}

TEST_CASE("UTF-8 encoding appends canonical scalar bytes", "[foundation][utf8]")
{
    const std::array cases{
        std::pair{char32_t{0x41U}, std::string{"A"}},
        std::pair{char32_t{0x7FU}, bytes({0x7FU})},
        std::pair{char32_t{0x80U}, bytes({0xC2U, 0x80U})},
        std::pair{char32_t{0x7FFU}, bytes({0xDFU, 0xBFU})},
        std::pair{char32_t{0x800U}, bytes({0xE0U, 0xA0U, 0x80U})},
        std::pair{char32_t{0xD7FFU}, bytes({0xEDU, 0x9FU, 0xBFU})},
        std::pair{char32_t{0xE000U}, bytes({0xEEU, 0x80U, 0x80U})},
        std::pair{char32_t{0xFFFFU}, bytes({0xEFU, 0xBFU, 0xBFU})},
        std::pair{char32_t{0x10000U}, bytes({0xF0U, 0x90U, 0x80U, 0x80U})},
        std::pair{char32_t{0x10FFFFU}, bytes({0xF4U, 0x8FU, 0xBFU, 0xBFU})},
    };

    for (const auto &[code_point, expected] : cases)
    {
        std::string output;
        REQUIRE(sparenode::text::utf8::append_code_point(output, code_point));
        CHECK(output == expected);
        CHECK(sparenode::text::utf8::is_valid(output));
    }

    for (const auto invalid : {char32_t{0xD800U}, char32_t{0xDFFFU}, char32_t{0x110000U}})
    {
        std::string output = "unchanged";
        CHECK_FALSE(sparenode::text::utf8::append_code_point(output, invalid));
        CHECK(output == "unchanged");
    }
}
