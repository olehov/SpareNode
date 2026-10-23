#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sparenode/foundation/result.hpp"

namespace sparenode::session
{

/// @brief Fixed one-way representation retained by the bounded session store.
using SessionCredentialDigest = std::array<std::byte, 32>;

/// @brief Owns a fresh client credential and its server-side digest.
struct GeneratedSessionCredential
{
    std::string value;                ///< Opaque value returned only to the caller.
    SessionCredentialDigest digest{}; ///< One-way value retained by the session store.
};

/// @brief Identifies a credential-codec failure without exposing secret material.
enum class SessionCredentialErrorCode : std::uint8_t
{
    invalid_credential,         ///< Submitted credential does not use the accepted encoding.
    cryptography_unavailable,   ///< Secure randomness or digest support is unavailable.
    resource_allocation_failed, ///< Credential storage could not be allocated.
};

/// @brief Generates opaque credentials and derives their stable one-way lookup digest.
///
/// Implementations must support concurrent calls because request workers share one codec.
class SessionCredentialCodec
{
  public:
    /// @brief Enables polymorphic destruction of concrete credential codecs.
    virtual ~SessionCredentialCodec() = default;

    /// @brief Produces one fresh credential with at least 256 bits of secure entropy.
    /// @return Client credential and retained digest, or a bounded codec failure.
    [[nodiscard]] virtual Result<GeneratedSessionCredential, SessionCredentialErrorCode>
    generate() const = 0;

    /// @brief Derives the server-side digest for one submitted credential.
    /// @param[in] credential Canonically encoded opaque client credential.
    /// @return Fixed digest, or an encoding or codec failure.
    [[nodiscard]] virtual Result<SessionCredentialDigest, SessionCredentialErrorCode>
    digest(std::string_view credential) const = 0;

  protected:
    /// @brief Allows construction only through concrete codec adapters.
    SessionCredentialCodec() = default;
    /// @brief Supports copying concrete codec adapters.
    SessionCredentialCodec(const SessionCredentialCodec &) = default;
    /// @brief Supports copy assignment for concrete codec adapters.
    /// @return This codec port.
    SessionCredentialCodec &operator=(const SessionCredentialCodec &) = default;
    /// @brief Supports moving concrete codec adapters.
    SessionCredentialCodec(SessionCredentialCodec &&) noexcept = default;
    /// @brief Supports move assignment for concrete codec adapters.
    /// @return This codec port.
    SessionCredentialCodec &operator=(SessionCredentialCodec &&) noexcept = default;
};

} // namespace sparenode::session
