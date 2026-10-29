#pragma once

#include <cstddef>
#include <span>

namespace sparenode::memory
{

/// @brief Overwrites a mutable byte range through volatile stores.
///
/// The volatile writes prevent the compiler from removing an erase whose result is otherwise
/// unobservable before the allocation is released or reused.
/// @param[in,out] bytes Sensitive bytes to clear while their storage is still alive.
void secure_erase(std::span<std::byte> bytes) noexcept;

/// @brief Overwrites mutable character storage through the same non-optimizable byte primitive.
/// @param[in,out] characters Sensitive textual storage to clear while still alive.
void secure_erase(std::span<char> characters) noexcept;

} // namespace sparenode::memory
