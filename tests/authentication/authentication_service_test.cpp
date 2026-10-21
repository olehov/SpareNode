#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/authentication/authentication_service.hpp"

namespace
{

/// @brief Deterministic cryptography-free verifier used to isolate authentication core tests.
class TestPasswordVerifier final : public sparenode::authentication::PasswordVerifier
{
  public:
    explicit TestPasswordVerifier(const bool fail_hash = false) noexcept : fail_hash_(fail_hash)
    {
    }

    [[nodiscard]] sparenode::Result<std::string,
                                    sparenode::authentication::PasswordVerifierErrorCode>
    hash(const std::span<const char> password) const override
    {
        if (fail_hash_)
        {
            return sparenode::unexpected(
                sparenode::authentication::PasswordVerifierErrorCode::hashing_failed);
        }
        return std::string("test:") + std::string(password.begin(), password.end());
    }

    [[nodiscard]] sparenode::Result<bool, sparenode::authentication::PasswordVerifierErrorCode>
    verify(const std::span<const char> password,
           const std::string_view encoded_verifier) const noexcept override
    {
        if (!is_usable(encoded_verifier))
        {
            return sparenode::unexpected(
                sparenode::authentication::PasswordVerifierErrorCode::invalid_verifier);
        }
        return encoded_verifier.substr(5) == std::string_view(password.data(), password.size());
    }

    [[nodiscard]] bool is_usable(const std::string_view encoded_verifier) const noexcept override
    {
        return encoded_verifier.starts_with("test:");
    }

  private:
    bool fail_hash_{};
};

/// @brief Controlled provider used to exercise source-independent authentication behavior.
class TestCredentialProvider final : public sparenode::authentication::CredentialProvider
{
  public:
    explicit TestCredentialProvider(sparenode::authentication::CredentialRecord credential)
        : credential_(std::move(credential))
    {
    }

    [[nodiscard]] sparenode::Result<std::optional<sparenode::authentication::CredentialRecord>,
                                    sparenode::authentication::CredentialProviderError>
    find_by_login(const std::string_view login) const override
    {
        if (fail_)
        {
            return sparenode::unexpected(sparenode::authentication::CredentialProviderError{
                sparenode::authentication::CredentialProviderErrorCode::unavailable});
        }
        if (login != credential_.canonical_login)
        {
            return std::optional<sparenode::authentication::CredentialRecord>{};
        }
        return std::optional<sparenode::authentication::CredentialRecord>{credential_};
    }

    void fail() noexcept
    {
        fail_ = true;
    }

  private:
    sparenode::authentication::CredentialRecord credential_;
    bool fail_{};
};

/// @brief Borrows a test password as the verifier's byte-oriented input.
[[nodiscard]] std::span<const char> password_bytes(const std::string_view password) noexcept
{
    return {password.data(), password.size()};
}

} // namespace

TEST_CASE("Authentication service rejects an unavailable verifier", "[authentication][service]")
{
    using CreateError = sparenode::authentication::AuthenticationServiceCreateErrorCode;
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});

    const std::shared_ptr<const sparenode::authentication::PasswordVerifier> missing_verifier;
    const auto missing =
        sparenode::authentication::AuthenticationService::create(provider, missing_verifier);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == CreateError::verifier_unavailable);

    const auto failing_verifier = std::make_shared<TestPasswordVerifier>(true);
    const auto failed =
        sparenode::authentication::AuthenticationService::create(provider, failing_verifier);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error() == CreateError::verifier_unavailable);
}

TEST_CASE("Authentication service verifies provider records and hides account existence",
          "[authentication][service]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    auto encoded = verifier->hash(password_bytes("owner password"));
    REQUIRE(encoded.has_value());
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", std::move(encoded).value()});
    auto service_result =
        sparenode::authentication::AuthenticationService::create(provider, verifier);
    REQUIRE(service_result.has_value());
    const auto service = std::move(service_result).value();

    const auto authenticated = service.authenticate(
        "admin", sparenode::authentication::SensitivePassword("owner password"));
    REQUIRE(authenticated.has_value());
    CHECK(authenticated->identity == "owner");
    CHECK(authenticated->canonical_login == "admin");

    const auto incorrect = service.authenticate(
        "admin", sparenode::authentication::SensitivePassword("wrong password"));
    REQUIRE_FALSE(incorrect.has_value());
    CHECK(incorrect.error() == Error::invalid_credentials);

    const auto unknown = service.authenticate(
        "unknown", sparenode::authentication::SensitivePassword("owner password"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == Error::invalid_credentials);

    provider->fail();
    const auto unavailable = service.authenticate(
        "admin", sparenode::authentication::SensitivePassword("owner password"));
    REQUIRE_FALSE(unavailable.has_value());
    CHECK(unavailable.error() == Error::unavailable);
}

TEST_CASE("Authentication service fails closed on malformed provider material",
          "[authentication][service]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "$argon2id$invalid"});
    auto service_result =
        sparenode::authentication::AuthenticationService::create(provider, verifier);
    REQUIRE(service_result.has_value());
    const auto service = std::move(service_result).value();

    const auto result = service.authenticate(
        "admin", sparenode::authentication::SensitivePassword("owner password"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::unavailable);
}
