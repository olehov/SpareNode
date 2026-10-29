#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

    void fail(const bool value = true) noexcept
    {
        fail_ = value;
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

TEST_CASE("Authentication service rejects an invalid rate limit policy",
          "[authentication][service][rate-limit]")
{
    using CreateError = sparenode::authentication::AuthenticationServiceCreateErrorCode;
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});
    const auto verifier = std::make_shared<TestPasswordVerifier>();

    const auto zero_failures = sparenode::authentication::AuthenticationService::create(
        provider, verifier, {.maximum_failures = 0, .cooldown = std::chrono::seconds{30}});
    REQUIRE_FALSE(zero_failures);
    CHECK(zero_failures.error() == CreateError::invalid_rate_limit_policy);

    const auto zero_cooldown = sparenode::authentication::AuthenticationService::create(
        provider, verifier, {.maximum_failures = 5, .cooldown = {}});
    REQUIRE_FALSE(zero_cooldown);
    CHECK(zero_cooldown.error() == CreateError::invalid_rate_limit_policy);
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
    CHECK(authenticated->principal.identity == "owner");
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

TEST_CASE("Authentication service throttles bounded failures and recovers after cooldown",
          "[authentication][service][rate-limit]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>();
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});
    auto service_result = sparenode::authentication::AuthenticationService::create(
        provider, verifier, {.maximum_failures = 2, .cooldown = std::chrono::seconds{30}},
        [now] { return *now; });
    REQUIRE(service_result);
    const auto service = std::move(service_result).value();

    for (const std::string_view login : {"admin", "unknown"})
    {
        const auto rejected =
            service.authenticate(login, sparenode::authentication::SensitivePassword("incorrect"));
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error() == Error::invalid_credentials);
    }

    const auto throttled =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(throttled);
    CHECK(throttled.error() == Error::throttled);

    *now += std::chrono::seconds{30} - std::chrono::steady_clock::duration{1};
    const auto still_throttled =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(still_throttled);
    CHECK(still_throttled.error() == Error::throttled);

    *now += std::chrono::steady_clock::duration{1};
    const auto recovered =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE(recovered);

    // A successful verification clears the shared counter before a new failure sequence.
    const auto first_new_failure =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("incorrect"));
    REQUIRE_FALSE(first_new_failure);
    CHECK(first_new_failure.error() == Error::invalid_credentials);
    const auto success_resets =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE(success_resets);
    const auto failure_after_reset =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("incorrect"));
    REQUIRE_FALSE(failure_after_reset);
    CHECK(failure_after_reset.error() == Error::invalid_credentials);
}

TEST_CASE("Authentication service does not spend failure budget on unavailable infrastructure",
          "[authentication][service][rate-limit]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});
    auto service_result = sparenode::authentication::AuthenticationService::create(
        provider, verifier, {.maximum_failures = 1, .cooldown = std::chrono::seconds{30}});
    REQUIRE(service_result);
    const auto service = std::move(service_result).value();

    provider->fail();
    const auto unavailable =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error() == Error::unavailable);

    provider->fail(false);
    const auto recovered =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE(recovered);
}

TEST_CASE("Authentication service applies one failure threshold to a concurrent burst",
          "[authentication][service][rate-limit][concurrency]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    constexpr std::size_t attempt_count = 8;
    constexpr std::size_t failure_limit = 2;
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});
    auto service_result = sparenode::authentication::AuthenticationService::create(
        provider, verifier,
        {.maximum_failures = failure_limit, .cooldown = std::chrono::seconds{30}});
    REQUIRE(service_result);
    const auto service = std::move(service_result).value();

    std::array<Error, attempt_count> outcomes{};
    std::vector<std::thread> attempts;
    attempts.reserve(attempt_count);
    for (std::size_t index = 0; index < attempt_count; ++index)
    {
        attempts.emplace_back(
            [&service, &outcomes, index]
            {
                const auto result = service.authenticate(
                    "admin", sparenode::authentication::SensitivePassword("incorrect"));
                outcomes[index] = result ? Error::unavailable : result.error();
            });
    }
    for (auto &attempt : attempts)
    {
        attempt.join();
    }

    CHECK(std::ranges::count(outcomes, Error::invalid_credentials) == failure_limit);
    CHECK(std::ranges::count(outcomes, Error::throttled) == attempt_count - failure_limit);
}

TEST_CASE("Authentication cooldown deadline saturates at the monotonic clock boundary",
          "[authentication][service][rate-limit]")
{
    using Error = sparenode::authentication::AuthenticationErrorCode;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point::max() - std::chrono::seconds{1});
    const auto verifier = std::make_shared<TestPasswordVerifier>();
    const auto provider = std::make_shared<TestCredentialProvider>(
        sparenode::authentication::CredentialRecord{"owner", "admin", "test:password"});
    auto service_result = sparenode::authentication::AuthenticationService::create(
        provider, verifier, {.maximum_failures = 1, .cooldown = std::chrono::seconds{30}},
        [now] { return *now; });
    REQUIRE(service_result);
    const auto service = std::move(service_result).value();

    const auto rejected =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("incorrect"));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == Error::invalid_credentials);

    const auto throttled =
        service.authenticate("admin", sparenode::authentication::SensitivePassword("password"));
    REQUIRE_FALSE(throttled);
    CHECK(throttled.error() == Error::throttled);
}
