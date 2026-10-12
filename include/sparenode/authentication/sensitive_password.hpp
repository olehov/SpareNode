#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace sparenode::authentication
{

/// @brief Overwrites a mutable plaintext buffer with a non-optimizable erase.
/// @param[in,out] bytes Sensitive bytes to erase before their storage is released.
void erase_sensitive_bytes(std::span<char> bytes) noexcept;

/// @brief Owns temporary plaintext password bytes and securely erases them on destruction.
class SensitivePassword final
{
  public:
    /// @brief Copies bounded password input into erasable owned storage.
    /// @param[in] password Temporary input retained only by this object after construction.
    explicit SensitivePassword(std::string_view password);

    /// @brief Adopts parser-owned password bytes without another plaintext copy.
    /// @param[in,out] password Temporary buffer transferred into secure lifetime management.
    explicit SensitivePassword(std::vector<char> password) noexcept;

    /// @brief Securely erases all retained plaintext bytes.
    ~SensitivePassword();

    SensitivePassword(const SensitivePassword &) = delete;
    SensitivePassword &operator=(const SensitivePassword &) = delete;

    /// @brief Transfers ownership of the erasable plaintext buffer.
    /// @param[in,out] other Password whose allocation is transferred.
    SensitivePassword(SensitivePassword &&other) noexcept;

    /// @brief Erases current bytes before taking another plaintext buffer.
    /// @param[in,out] other Password whose allocation is transferred.
    /// @return This password owner.
    SensitivePassword &operator=(SensitivePassword &&other) noexcept;

    /// @brief Borrows plaintext bytes for one immediate verification operation.
    /// @return Read-only bytes valid until this object is moved or destroyed.
    [[nodiscard]] std::span<const char> bytes() const noexcept;

    /// @brief Compares two plaintext buffers without content-dependent early exit.
    /// @param[in] other Password to compare against this instance.
    /// @return `true` only when both byte sequences are identical.
    [[nodiscard]] bool equals(const SensitivePassword &other) const noexcept;

  private:
    /// @brief Overwrites the active plaintext allocation without releasing ownership.
    void erase() noexcept;

    std::vector<char> bytes_; ///< Heap-backed bytes without small-string copies.
};

} // namespace sparenode::authentication
