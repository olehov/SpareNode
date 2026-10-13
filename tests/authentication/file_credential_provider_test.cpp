#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sparenode/authentication/file_authentication.hpp"
#include "sparenode/authentication/file_credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/configuration/runtime/location_config.hpp"
#include "sparenode/configuration/runtime/location_permissions.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Borrows a test password as the verifier's byte-oriented input.
[[nodiscard]] std::span<const char> password_bytes(const std::string_view password) noexcept
{
    return {password.data(), password.size()};
}

/// @brief Builds one valid credential source around a generated verifier.
[[nodiscard]] std::string credential_source(const std::string_view login,
                                            const std::string_view encoded_verifier)
{
    return "owner {\n    login \"" + std::string(login) + "\";\n    password_verifier \"" +
           std::string(encoded_verifier) + "\";\n}\n";
}

} // namespace

TEST_CASE("File credential provider loads one validated owner record",
          "[authentication][credentials]")
{
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();
    auto encoded = verifier.hash(password_bytes("owner password"));
    REQUIRE(encoded.has_value());

    const sparenode::test::TemporaryDirectory directory("sparenode-owner-credential");
    const auto path = directory.path() / "credentials.conf";
    {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.is_open());
        output << credential_source("admin", encoded.value());
        REQUIRE(output.good());
    }

    auto provider_result = sparenode::authentication::FileCredentialProvider::load(path, verifier);
    REQUIRE(provider_result.has_value());
    const auto provider = std::move(provider_result).value();

    const auto owner = provider.find_by_login("admin");
    REQUIRE(owner.has_value());
    const auto &record = sparenode::test::require_optional(owner.value());
    CHECK(record.identity == "owner");
    CHECK(record.canonical_login == "admin");
    CHECK(verifier.is_usable(record.encoded_password_verifier));

    const auto unknown = provider.find_by_login("Admin");
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->has_value());
}

TEST_CASE("File credential provider rejects missing and malformed material",
          "[authentication][credentials]")
{
    using Error = sparenode::authentication::CredentialProvisioningErrorCode;
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();
    auto encoded = verifier.hash(password_bytes("owner password"));
    REQUIRE(encoded.has_value());

    SECTION("missing file")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::load(
            "missing-owner-credentials.conf", verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::open_failed);
    }
    SECTION("non-regular source")
    {
        const sparenode::test::TemporaryDirectory directory("sparenode-owner-non-regular");
        const auto result =
            sparenode::authentication::FileCredentialProvider::load(directory.path(), verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::not_regular_file);
    }
    SECTION("missing login")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            "owner { password_verifier \"" + encoded.value() + "\"; }", verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::missing_login);
    }
    SECTION("missing verifier")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            "owner { login \"admin\"; }", verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::missing_verifier);
    }
    SECTION("duplicate directive")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            "owner { login \"admin\"; login \"other\"; password_verifier \"" + encoded.value() +
                "\"; }",
            verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::malformed_file);
    }
    SECTION("unknown directive")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            "owner { name \"admin\"; password_verifier \"" + encoded.value() + "\"; }", verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::malformed_file);
    }
    SECTION("invalid login")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            credential_source("", encoded.value()), verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::invalid_login);
    }
    SECTION("invalid verifier")
    {
        const auto result = sparenode::authentication::FileCredentialProvider::create_from_source(
            credential_source("admin", "$argon2id$invalid"), verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::invalid_verifier);
    }
    SECTION("oversized source")
    {
        const std::string source(sparenode::authentication::maximum_credential_file_bytes + 1, 'x');
        const auto result =
            sparenode::authentication::FileCredentialProvider::create_from_source(source, verifier);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == Error::file_too_large);
    }
}

TEST_CASE("File credential provider rejects a symbolic-link source",
          "[authentication][credentials][security][symlink]")
{
    using Error = sparenode::authentication::CredentialProvisioningErrorCode;
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();
    auto encoded = verifier.hash(password_bytes("owner password"));
    REQUIRE(encoded.has_value());

    const sparenode::test::TemporaryDirectory directory("sparenode-owner-symlink");
    const auto target = directory.path() / "target.conf";
    {
        std::ofstream output(target, std::ios::binary);
        REQUIRE(output.is_open());
        output << credential_source("admin", encoded.value());
        REQUIRE(output.good());
    }
    const auto link = directory.path() / "credentials.conf";
    std::error_code link_error;
    std::filesystem::create_symlink(target, link, link_error);
    if (link_error)
    {
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
    }

    const auto result = sparenode::authentication::FileCredentialProvider::load(link, verifier);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::not_regular_file);
}

TEST_CASE("File authentication rejects credentials inside a shared location",
          "[authentication][credentials][security]")
{
    auto verifier_result = sparenode::authentication::PasswordVerifier::create();
    REQUIRE(verifier_result.has_value());
    const auto verifier = verifier_result.value();
    auto encoded = verifier.hash(password_bytes("owner password"));
    REQUIRE(encoded.has_value());

    const sparenode::test::TemporaryDirectory directory("sparenode-shared-owner-credential");
    const auto credential_path = directory.path() / "credentials.conf";
    {
        std::ofstream output(credential_path, std::ios::binary);
        REQUIRE(output.is_open());
        output << credential_source("admin", encoded.value());
        REQUIRE(output.good());
    }
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root.has_value());
    std::vector<sparenode::configuration::runtime::LocationConfig> locations;
    locations.emplace_back(
        "/api/Documents", std::move(root).value(),
        sparenode::configuration::runtime::LocationPermissions{true, false, false});

    const auto result =
        sparenode::authentication::create_file_authentication_service(credential_path, locations);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() ==
          sparenode::authentication::FileAuthenticationErrorCode::credential_is_shared);
}
