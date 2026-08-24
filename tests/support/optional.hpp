#pragma once

#include <optional>
#include <stdexcept>

namespace sparenode::test
{

/// @brief Returns a mutable optional value after enforcing its presence for analyzers.
/// @tparam Value Stored value type.
/// @param[in,out] optional Optional whose value is required by the test.
/// @return Mutable reference to the stored value.
/// @throws std::logic_error When the optional is empty.
template <typename Value> [[nodiscard]] Value &require_optional(std::optional<Value> &optional)
{
    if (!optional.has_value())
    {
        throw std::logic_error("Expected optional test value to be present");
    }
    return optional.value();
}

/// @brief Returns an immutable optional value after enforcing its presence for analyzers.
/// @tparam Value Stored value type.
/// @param[in] optional Optional whose value is required by the test.
/// @return Immutable reference to the stored value.
/// @throws std::logic_error When the optional is empty.
template <typename Value>
[[nodiscard]] const Value &require_optional(const std::optional<Value> &optional)
{
    if (!optional.has_value())
    {
        throw std::logic_error("Expected optional test value to be present");
    }
    return optional.value();
}

} // namespace sparenode::test
