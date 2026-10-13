#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/foundation/result.hpp"

namespace sparenode::authentication
{

/// @brief Stores password-verification material without containing a plaintext password.
struct CredentialRecord
{
    std::string identity;                  ///< Opaque identity used after authentication.
    std::string canonical_login;           ///< Provider-owned canonical login identifier.
    std::string encoded_password_verifier; ///< Algorithm, parameters, salt, and password hash.
};

/// @brief Identifies why a credential provider could not complete a lookup.
enum class CredentialProviderErrorCode : std::uint8_t
{
    unavailable,        ///< The backing credential source could not answer.
    resource_exhausted, ///< The lookup could not allocate bounded result storage.
};

/// @brief Reports a provider failure without carrying credential material.
struct CredentialProviderError
{
    CredentialProviderErrorCode code{}; ///< Stable non-sensitive failure category.
};

/// @brief Abstracts credential storage from authentication and HTTP layers.
class CredentialProvider
{
  public:
    virtual ~CredentialProvider() = default;
    CredentialProvider() = default;
    CredentialProvider(const CredentialProvider &) = delete;
    CredentialProvider &operator=(const CredentialProvider &) = delete;

    /// @brief Moves the stateless provider base into a concrete provider owner.
    /// @param[in,out] other Provider base being transferred.
    CredentialProvider(CredentialProvider &&other) noexcept = default;

    /// @brief Replaces the stateless provider base during concrete provider movement.
    /// @param[in,out] other Provider base being transferred.
    /// @return This provider base.
    CredentialProvider &operator=(CredentialProvider &&other) noexcept = default;

    /// @brief Looks up verification material for one submitted login identifier.
    /// @param[in] login Untrusted bounded identifier supplied by an authentication client.
    /// @return A credential when found, no value when unknown, or a provider failure.
    [[nodiscard]] virtual Result<std::optional<CredentialRecord>, CredentialProviderError>
    find_by_login(std::string_view login) const = 0;
};

} // namespace sparenode::authentication
