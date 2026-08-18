#pragma once

#include <cerrno>

namespace sparenode::network::detail
{

/// @brief Captures either a native operation result or its terminal error code.
/// @tparam Value Native operation result type.
template <typename Value> struct InterruptedOperationResult
{
    /// @brief Native operation result or its invalid sentinel.
    Value value;
    /// @brief Zero after success, otherwise the terminal native error code.
    int error_code;
};

/// @brief Repeats an operation while its error is classified as an interruption.
///
/// The operation, error provider, and retry predicate are injected so the retry
/// control flow can be tested deterministically without signals or timing.
///
/// @tparam Value Native operation result type.
/// @tparam Operation Nullary callable that performs the native operation.
/// @tparam ErrorProvider Nullary callable that returns the latest error code.
/// @tparam RetryPredicate Callable deciding whether an error is retryable.
/// @param[in] invalid_value Sentinel returned by a failed native operation.
/// @param[in] operation Operation invoked until success or a terminal error.
/// @param[in] error_provider Provider queried after each failed attempt.
/// @param[in] should_retry Predicate applied to each reported error code.
/// @return The successful value with code zero, or the sentinel and terminal code.
template <typename Value, typename Operation, typename ErrorProvider, typename RetryPredicate>
[[nodiscard]] InterruptedOperationResult<Value>
retry_interrupted_operation(const Value invalid_value, Operation operation,
                            ErrorProvider error_provider, RetryPredicate should_retry)
{
    while (true)
    {
        const Value value = operation();
        if (value != invalid_value)
        {
            return {value, 0};
        }

        const int error_code = error_provider();
        if (!should_retry(error_code))
        {
            return {invalid_value, error_code};
        }
    }
}

/// @brief Reports whether a failed native accept should be attempted again.
/// @param[in] error_code Platform socket error reported by accept.
/// @return `true` when retrying is safe on the current platform.
[[nodiscard]] constexpr bool should_retry_accept(const int error_code) noexcept
{
#ifdef _WIN32
    static_cast<void>(error_code);
    return false;
#else
    return error_code == EINTR;
#endif
}

} // namespace sparenode::network::detail
