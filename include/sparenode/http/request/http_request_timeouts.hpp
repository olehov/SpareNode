#pragma once

#include <chrono>

namespace sparenode::http
{

/// @brief Groups receive budgets for one HTTP request, independent of parser storage limits.
struct HttpRequestTimeouts
{
    /// @brief Largest timeout accepted by the persistent configuration, in milliseconds.
    static constexpr std::chrono::milliseconds maximum_config_timeout{std::chrono::hours{24}};

    std::chrono::milliseconds headers{std::chrono::seconds{10}}; ///< Header read inactivity limit.
    std::chrono::milliseconds body{std::chrono::seconds{10}};    ///< Body read inactivity limit.
    std::chrono::milliseconds total{std::chrono::seconds{30}};   ///< Non-renewable receive budget.
};

} // namespace sparenode::http
