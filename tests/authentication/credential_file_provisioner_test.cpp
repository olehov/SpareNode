#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <utility>

#include "sparenode/authentication/credential_file_provisioner.hpp"
#include "sparenode/authentication/file_credential_provider.hpp"
#include "sparenode/authentication/sensitive_password.hpp"
#include "sparenode/authentication/sodium_password_verifier.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Credential file provisioner creates a loadable persistent owner",
          "[authentication][credentials][provisioning]")
{
    auto verifier_result = sparenode::authentication::SodiumPasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();

    const sparenode::test::TemporaryDirectory directory("sparenode-credential-provisioning");
    const auto path = directory.path() / "credentials.conf";
    const std::string login = R"(admin"backup\local)";
    const std::string password = "persistent owner password";

    const auto provisioned = sparenode::authentication::CredentialFileProvisioner::provision(
        path, login, sparenode::authentication::SensitivePassword(password), verifier);
    REQUIRE(provisioned.has_value());

    auto provider_result = sparenode::authentication::FileCredentialProvider::load(path, verifier);
    REQUIRE(provider_result.has_value());
    const auto provider = std::move(provider_result).value();
    const auto credential_result = provider.find_by_login(login);
    REQUIRE(credential_result.has_value());
    const auto &credential = sparenode::test::require_optional(credential_result.value());
    CHECK(credential.canonical_login == login);

    const auto verified = verifier.verify(std::span<const char>{password.data(), password.size()},
                                          credential.encoded_password_verifier);
    REQUIRE(verified.has_value());
    CHECK(verified.value());
}

TEST_CASE("Credential file provisioner refuses unsafe input and replacement",
          "[authentication][credentials][provisioning]")
{
    using Error = sparenode::authentication::CredentialFileProvisioningErrorCode;
    auto verifier_result = sparenode::authentication::SodiumPasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();

    const sparenode::test::TemporaryDirectory directory("sparenode-credential-boundaries");
    const auto path = directory.path() / "credentials.conf";

    const auto invalid_login = sparenode::authentication::CredentialFileProvisioner::provision(
        path, "", sparenode::authentication::SensitivePassword("password"), verifier);
    REQUIRE_FALSE(invalid_login.has_value());
    CHECK(invalid_login.error() == Error::invalid_login);

    const auto empty_password = sparenode::authentication::CredentialFileProvisioner::provision(
        path, "admin", sparenode::authentication::SensitivePassword(""), verifier);
    REQUIRE_FALSE(empty_password.has_value());
    CHECK(empty_password.error() == Error::empty_password);

    const auto first = sparenode::authentication::CredentialFileProvisioner::provision(
        path, "admin", sparenode::authentication::SensitivePassword("first password"), verifier);
    REQUIRE(first.has_value());
    const auto replacement = sparenode::authentication::CredentialFileProvisioner::provision(
        path, "admin", sparenode::authentication::SensitivePassword("second password"), verifier);
    REQUIRE_FALSE(replacement.has_value());
    CHECK(replacement.error() == Error::output_exists);

    auto provider_result = sparenode::authentication::FileCredentialProvider::load(path, verifier);
    REQUIRE(provider_result.has_value());
    const auto provider = std::move(provider_result).value();
    const auto credential_result = provider.find_by_login("admin");
    REQUIRE(credential_result.has_value());
    const auto &credential = sparenode::test::require_optional(credential_result.value());
    const std::string first_password = "first password";
    const auto verified =
        verifier.verify(std::span<const char>{first_password.data(), first_password.size()},
                        credential.encoded_password_verifier);
    REQUIRE(verified.has_value());
    CHECK(verified.value());
}
