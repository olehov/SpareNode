#include <catch2/catch_test_macros.hpp>

#include <string>

#include "sparenode/session/sodium_session_credential_codec.hpp"

TEST_CASE("Sodium session codec generates unique 256-bit opaque credentials", "[session][sodium]")
{
    auto codec = sparenode::session::SodiumSessionCredentialCodec::create();
    REQUIRE(codec);
    auto first = codec->generate();
    auto second = codec->generate();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->value.size() == 64);
    CHECK(second->value.size() == 64);
    CHECK(first->value != second->value);
    CHECK(first->digest != second->digest);
    REQUIRE(codec->digest(first->value));
    CHECK(codec->digest(first->value).value() == first->digest);
}

TEST_CASE("Sodium session codec accepts only canonical credentials", "[session][sodium]")
{
    auto codec = sparenode::session::SodiumSessionCredentialCodec::create();
    REQUIRE(codec);
    CHECK_FALSE(codec->digest("short"));
    CHECK_FALSE(codec->digest(std::string(64, 'G')));
    CHECK_FALSE(codec->digest(std::string(64, 'A')));
}
