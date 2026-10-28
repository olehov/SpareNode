#include <algorithm>
#include <array>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/memory/secure_erase.hpp"

TEST_CASE("Secure erase clears every byte in the borrowed range", "[foundation][security]")
{
    std::array bytes{std::byte{0x01}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}};

    sparenode::memory::secure_erase(bytes);

    CHECK(std::ranges::all_of(bytes, [](const auto byte) { return byte == std::byte{}; }));
}

TEST_CASE("Secure erase supports mutable character storage", "[foundation][security]")
{
    std::array characters{'s', 'e', 'c', 'r', 'e', 't'};

    sparenode::memory::secure_erase(characters);

    CHECK(std::ranges::all_of(characters, [](const char value) { return value == '\0'; }));
}
