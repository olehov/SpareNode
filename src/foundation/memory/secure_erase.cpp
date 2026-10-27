#include "sparenode/foundation/memory/secure_erase.hpp"

namespace sparenode::memory
{

void secure_erase(const std::span<std::byte> bytes) noexcept
{
    volatile std::byte *current = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        current[index] = std::byte{};
    }
}

} // namespace sparenode::memory
