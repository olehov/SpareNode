#include "sparenode/authentication/sensitive_password.hpp"

#include <cstddef>
#include <utility>

#include "sparenode/foundation/memory/secure_erase.hpp"

namespace sparenode::authentication
{

void erase_sensitive_bytes(const std::span<char> bytes) noexcept
{
    memory::secure_erase(std::as_writable_bytes(bytes));
}

SensitivePassword::SensitivePassword(const std::string_view password)
    : bytes_(password.begin(), password.end())
{
}

SensitivePassword::SensitivePassword(std::vector<char> password) noexcept
    : bytes_(std::move(password))
{
}

SensitivePassword::~SensitivePassword()
{
    erase();
}

SensitivePassword::SensitivePassword(SensitivePassword &&other) noexcept
    : bytes_(std::move(other.bytes_))
{
}

SensitivePassword &SensitivePassword::operator=(SensitivePassword &&other) noexcept
{
    if (this != &other)
    {
        erase();
        bytes_ = std::move(other.bytes_);
    }
    return *this;
}

std::span<const char> SensitivePassword::bytes() const noexcept
{
    return bytes_;
}

bool SensitivePassword::equals(const SensitivePassword &other) const noexcept
{
    if (bytes_.size() != other.bytes_.size())
    {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < bytes_.size(); ++index)
    {
        difference |= static_cast<unsigned char>(bytes_[index]) ^
                      static_cast<unsigned char>(other.bytes_[index]);
    }
    return difference == 0;
}

void SensitivePassword::erase() noexcept
{
    erase_sensitive_bytes(bytes_);
}

} // namespace sparenode::authentication
