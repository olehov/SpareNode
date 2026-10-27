#include <array>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/text/hexadecimal.hpp"

TEST_CASE("Hexadecimal digit conversion accepts both ASCII letter cases",
          "[foundation][hexadecimal]")
{
    const std::array cases{
        std::pair{'0', std::uint8_t{0}},  std::pair{'9', std::uint8_t{9}},
        std::pair{'a', std::uint8_t{10}}, std::pair{'f', std::uint8_t{15}},
        std::pair{'A', std::uint8_t{10}}, std::pair{'F', std::uint8_t{15}},
    };

    for (const auto &[character, expected] : cases)
    {
        CHECK(sparenode::text::hexadecimal::digit_value(character) == expected);
    }
}

TEST_CASE("Hexadecimal digit conversion rejects non-hexadecimal bytes", "[foundation][hexadecimal]")
{
    constexpr std::array invalid{
        '\0', '/', ':', '@', '`', 'g', 'G', static_cast<char>(0x80), static_cast<char>(0xFF)};
    for (const auto character : invalid)
    {
        CHECK(!sparenode::text::hexadecimal::digit_value(character));
    }
}
