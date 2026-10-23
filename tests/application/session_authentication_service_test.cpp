#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/application/session_authentication_service.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Provides deterministic password verification for application-service tests.
class TestPasswordVerifier final : public sparenode::authentication::PasswordVerifier
{
  public:
    /// @brief Encodes plaintext in a test-only recognizable form.
    [[nodiscard]] sparenode::Result<std::string,
                                    sparenode::authentication::PasswordVerifierErrorCode>
    hash(const std::span<const char> password) const override
    {
        return std::string("test:") + std::string(password.begin(), password.end());
    }

    /// @brief Compares plaintext with the deterministic encoded test value.
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

    /// @brief Accepts only values produced by the test hash operation.
    [[nodiscard]] bool is_usable(const std::string_view encoded_verifier) const noexcept override
    {
        return encoded_verifier.starts_with("test:");
    }
};

/// @brief Supplies one owner credential and an optional fail-closed lookup mode.
class TestCredentialProvider final : public sparenode::authentication::CredentialProvider
{
  public:
    /// @brief Returns the owner record, no match, or the configured provider failure.
    [[nodiscard]] sparenode::Result<std::optional<sparenode::authentication::CredentialRecord>,
                                    sparenode::authentication::CredentialProviderError>
    find_by_login(const std::string_view login) const override
    {
        if (fail_)
        {
            return sparenode::unexpected(sparenode::authentication::CredentialProviderError{
                sparenode::authentication::CredentialProviderErrorCode::unavailable});
        }
        if (login != "admin")
        {
            return std::optional<sparenode::authentication::CredentialRecord>{};
        }
        return std::optional<sparenode::authentication::CredentialRecord>{
            sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"}};
    }

    /// @brief Makes subsequent credential lookups fail closed.
    void fail() noexcept
    {
        fail_ = true;
    }

  private:
    bool fail_{};
};

/// @brief Generates deterministic unique credentials and injectable codec failures.
class TestSessionCodec final : public sparenode::session::SessionCredentialCodec
{
  public:
    /// @brief Configures independent generation and digest failures.
    explicit TestSessionCodec(const bool fail_generation = false,
                              const bool fail_digest = false) noexcept
        : fail_generation_(fail_generation), fail_digest_(fail_digest)
    {
    }

    /// @brief Generates the next deterministic session credential.
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        if (fail_generation_)
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::cryptography_unavailable);
        }
        const auto value = std::string("session-") +
                           std::to_string(sequence_.fetch_add(1, std::memory_order_relaxed));
        return sparenode::session::GeneratedSessionCredential{value, digest_for(value)};
    }

    /// @brief Derives the deterministic digest or reports the configured failure.
    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const override
    {
        if (fail_digest_)
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::cryptography_unavailable);
        }
        if (!credential.starts_with("session-"))
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::invalid_credential);
        }
        return digest_for(credential);
    }

  private:
    /// @brief Folds credential bytes into a stable test digest.
    [[nodiscard]] static sparenode::session::SessionCredentialDigest
    digest_for(const std::string_view credential)
    {
        sparenode::session::SessionCredentialDigest digest{};
        for (std::size_t index = 0; index < credential.size(); ++index)
        {
            digest[index % digest.size()] ^= static_cast<std::byte>(credential[index]);
        }
        return digest;
    }

    bool fail_generation_{};
    bool fail_digest_{};
    mutable std::atomic_size_t sequence_{};
};

struct TestService
{
    std::shared_ptr<TestCredentialProvider> provider;
    sparenode::application::SessionAuthenticationService service;
};

/// @brief Creates the application service with deterministic credential and session adapters.
[[nodiscard]] TestService make_service(const std::shared_ptr<TestSessionCodec> &codec,
                                       const std::size_t maximum_sessions = 64)
{
    auto provider = std::make_shared<TestCredentialProvider>();
    auto authentication = sparenode::authentication::AuthenticationService::create(
        provider, std::make_shared<TestPasswordVerifier>());
    REQUIRE(authentication);
    auto sessions = sparenode::session::SessionStore::create(
        codec, {.absolute_lifetime = std::chrono::hours{12},
                .idle_timeout = std::chrono::minutes{30},
                .maximum_sessions = maximum_sessions});
    REQUIRE(sessions);
    auto service = sparenode::application::SessionAuthenticationService::create(
        std::move(authentication).value(), std::move(sessions).value());
    REQUIRE(service);
    return {std::move(provider), std::move(service).value()};
}

} // namespace

TEST_CASE("Session authentication service creates a bounded session and logs it out",
          "[application][authentication][session]")
{
    auto fixture = make_service(std::make_shared<TestSessionCodec>(), 1);
    auto first =
        fixture.service.login("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE(first);
    auto second =
        fixture.service.login("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE(second);
    CHECK(first.value() != second.value());
    CHECK(fixture.service.sessions()->size() == 1);

    auto evicted = fixture.service.sessions()->authenticate(first.value());
    REQUIRE(evicted);
    CHECK_FALSE(evicted->has_value());
    auto active = fixture.service.sessions()->authenticate(second.value());
    REQUIRE(active);
    REQUIRE(active->has_value());
    CHECK(sparenode::test::require_optional(active.value()).identity == "owner");

    auto logged_out = fixture.service.logout(second.value());
    REQUIRE(logged_out);
    REQUIRE(logged_out->has_value());
    CHECK(sparenode::test::require_optional(logged_out.value()).identity == "owner");
    auto rejected = fixture.service.sessions()->authenticate(second.value());
    REQUIRE(rejected);
    CHECK_FALSE(rejected->has_value());
    auto repeated = fixture.service.logout(second.value());
    REQUIRE(repeated);
    CHECK_FALSE(repeated->has_value());
}

TEST_CASE("Session authentication service preserves generic fail-closed outcomes",
          "[application][authentication][session]")
{
    using LoginError = sparenode::application::SessionLoginErrorCode;
    auto fixture = make_service(std::make_shared<TestSessionCodec>());

    auto incorrect =
        fixture.service.login("admin", sparenode::authentication::SensitivePassword("incorrect"));
    REQUIRE_FALSE(incorrect);
    CHECK(incorrect.error() == LoginError::invalid_credentials);

    fixture.provider->fail();
    auto unavailable =
        fixture.service.login("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error() == LoginError::unavailable);

    auto generation_failure = make_service(std::make_shared<TestSessionCodec>(true));
    auto not_created = generation_failure.service.login(
        "admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(not_created);
    CHECK(not_created.error() == LoginError::unavailable);

    auto digest_failure = make_service(std::make_shared<TestSessionCodec>(false, true));
    auto not_invalidated = digest_failure.service.logout("session-0");
    REQUIRE_FALSE(not_invalidated);
    CHECK(not_invalidated.error() == sparenode::application::SessionLogoutErrorCode::unavailable);
}
