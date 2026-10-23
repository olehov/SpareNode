#pragma once

#include "sparenode/foundation/result.hpp"
#include "sparenode/session/session_credential_codec.hpp"

namespace sparenode::session
{

/// @brief Implements opaque session credentials with libsodium randomness and hashing.
class SodiumSessionCredentialCodec final : public SessionCredentialCodec
{
  public:
    /// @brief Initializes libsodium before exposing the codec.
    /// @return Ready codec, or a cryptographic initialization failure.
    [[nodiscard]] static Result<SodiumSessionCredentialCodec, SessionCredentialErrorCode> create();

    /// @brief Copies this stateless libsodium adapter.
    SodiumSessionCredentialCodec(const SodiumSessionCredentialCodec &) = default;
    /// @brief Copy-assigns this stateless libsodium adapter.
    /// @return This adapter.
    SodiumSessionCredentialCodec &operator=(const SodiumSessionCredentialCodec &) = default;
    /// @brief Moves this stateless libsodium adapter.
    SodiumSessionCredentialCodec(SodiumSessionCredentialCodec &&) noexcept = default;
    /// @brief Move-assigns this stateless libsodium adapter.
    /// @return This adapter.
    SodiumSessionCredentialCodec &operator=(SodiumSessionCredentialCodec &&) noexcept = default;

    /// @copydoc SessionCredentialCodec::generate
    [[nodiscard]] Result<GeneratedSessionCredential, SessionCredentialErrorCode>
    generate() const override;

    /// @copydoc SessionCredentialCodec::digest
    [[nodiscard]] Result<SessionCredentialDigest, SessionCredentialErrorCode>
    digest(std::string_view credential) const override;

  private:
    /// @brief Restricts construction to the checked `create` factory.
    SodiumSessionCredentialCodec() = default;
};

} // namespace sparenode::session
