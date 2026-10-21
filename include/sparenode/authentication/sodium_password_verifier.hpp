#pragma once

#include "sparenode/authentication/password_verifier.hpp"

namespace sparenode::authentication
{

/// @brief Implements the password-verification port with libsodium Argon2id.
class SodiumPasswordVerifier final : public PasswordVerifier
{
  public:
    /// @brief Initializes the process-wide vetted cryptographic runtime.
    /// @return Ready verifier, or an initialization failure.
    [[nodiscard]] static Result<SodiumPasswordVerifier, PasswordVerifierErrorCode>
    create() noexcept;

    /// @brief Copies this stateless libsodium adapter.
    SodiumPasswordVerifier(const SodiumPasswordVerifier &) = default;
    /// @brief Copy-assigns this stateless libsodium adapter.
    /// @return This adapter.
    SodiumPasswordVerifier &operator=(const SodiumPasswordVerifier &) = default;
    /// @brief Moves this stateless libsodium adapter.
    SodiumPasswordVerifier(SodiumPasswordVerifier &&) noexcept = default;
    /// @brief Move-assigns this stateless libsodium adapter.
    /// @return This adapter.
    SodiumPasswordVerifier &operator=(SodiumPasswordVerifier &&) noexcept = default;

    /// @copydoc PasswordVerifier::hash
    [[nodiscard]] Result<std::string, PasswordVerifierErrorCode>
    hash(std::span<const char> password) const override;

    /// @copydoc PasswordVerifier::verify
    [[nodiscard]] Result<bool, PasswordVerifierErrorCode>
    verify(std::span<const char> password,
           std::string_view encoded_verifier) const noexcept override;

    /// @copydoc PasswordVerifier::is_usable
    [[nodiscard]] bool is_usable(std::string_view encoded_verifier) const noexcept override;

  private:
    /// @brief Restricts construction to the checked `create` factory.
    SodiumPasswordVerifier() = default;
};

} // namespace sparenode::authentication
