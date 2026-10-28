#include <array>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/http/detail/ascii.hpp"

TEST_CASE("HTTP ASCII helpers implement token and case rules", "[http][ascii]")
{
    using namespace sparenode::http::detail;

    const std::array lowercase_cases{std::pair{'A', 'a'}, std::pair{'-', '-'}};
    for (const auto &[input, expected] : lowercase_cases)
    {
        CHECK(ascii_lower(input) == expected);
    }
    CHECK(ascii_case_insensitive_equal("Content-Type", "content-type"));
    CHECK(!ascii_case_insensitive_equal("Host", "Hosts"));

    for (const char byte : std::string_view{"AZaz09!#$%&'*+-.^_`|~"})
    {
        CHECK(is_http_token_character(byte));
    }
    for (const char byte : std::string_view{" ()/:;<=>?@[\\]{}"})
    {
        CHECK(!is_http_token_character(byte));
    }
}

TEST_CASE("HTTP field values reject framing control bytes", "[http][ascii]")
{
    using sparenode::http::detail::is_http_field_value_byte;
    constexpr std::array<unsigned char, 5> permitted{'\t', ' ', '~', 0x80U, 0xFFU};
    constexpr std::array<unsigned char, 3> rejected{0x00U, 0x1FU, 0x7FU};

    for (const auto byte : permitted)
    {
        CHECK(is_http_field_value_byte(byte));
    }
    for (const auto byte : rejected)
    {
        CHECK(!is_http_field_value_byte(byte));
    }
}

TEST_CASE("HTTP optional whitespace trimming removes only SP and HTAB", "[http][ascii]")
{
    using sparenode::http::detail::trim_optional_whitespace;

    CHECK(trim_optional_whitespace(" \t value \t ") == "value");
    CHECK(trim_optional_whitespace("\t\t") == "");
    CHECK(trim_optional_whitespace("\r value \n") == "\r value \n");
}
