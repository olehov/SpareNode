#pragma once

#include <string>

namespace sparenode::filesystem::detail
{

/// @brief Suggests an unpredictable filename suffix; native exclusive creation ensures uniqueness.
/// @return Hexadecimal candidate used only with an exclusive create operation.
[[nodiscard]] std::string temporary_name_suffix();

} // namespace sparenode::filesystem::detail
