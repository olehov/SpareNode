#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>

#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/authentication/sensitive_password.hpp"

namespace
{

/// @brief Borrows a test password as the verifier's byte-oriented input.
[[nodiscard]] std::span<const char> password_bytes(const std::string_view password) noexcept
{
    return {password.data(), password.size()};
}

} // namespace

TEST_CASE("Password verifier creates and checks self-contained Argon2id records",
          "[authentication][password]")
{
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();

    const auto first = verifier.hash(password_bytes("correct horse battery staple"));
    const auto second = verifier.hash(password_bytes("correct horse battery staple"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->starts_with("$argon2id$"));
    CHECK(verifier.is_usable(first.value()));
    CHECK(first.value() != second.value());

    const auto correct =
        verifier.verify(password_bytes("correct horse battery staple"), first.value());
    const auto incorrect = verifier.verify(password_bytes("incorrect"), first.value());
    REQUIRE(correct.has_value());
    REQUIRE(incorrect.has_value());
    CHECK(correct.value());
    CHECK_FALSE(incorrect.value());
}

TEST_CASE("Password verifier rejects unsafe provisioning material", "[authentication][password]")
{
    using Error = sparenode::authentication::PasswordVerifierErrorCode;
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();

    const auto empty = verifier.hash(password_bytes(""));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == Error::empty_password);

    const std::string oversized(sparenode::authentication::maximum_password_bytes + 1, 'x');
    const auto too_large = verifier.hash(password_bytes(oversized));
    REQUIRE_FALSE(too_large.has_value());
    CHECK(too_large.error() == Error::password_too_large);

    const auto malformed = verifier.verify(password_bytes("password"), "$argon2id$invalid");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error() == Error::invalid_verifier);
    CHECK_FALSE(verifier.is_usable("$argon2i$v=19$m=65536,t=2,p=1$invalid$invalid"));
    CHECK_FALSE(verifier.is_usable("$argon2id$v=19$m=65536,t=2,p=2$invalid$invalid"));
}

TEST_CASE("Sensitive passwords compare complete plaintext buffers",
          "[authentication][password][sensitive]")
{
    const sparenode::authentication::SensitivePassword password("same password");
    const sparenode::authentication::SensitivePassword matching("same password");
    const sparenode::authentication::SensitivePassword different("different password");
    const sparenode::authentication::SensitivePassword shorter("same passwor");

    CHECK(password.equals(matching));
    CHECK_FALSE(password.equals(different));
    CHECK_FALSE(password.equals(shorter));
}
