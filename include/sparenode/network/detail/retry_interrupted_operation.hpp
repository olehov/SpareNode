#pragma once

#include <cerrno>

namespace sparenode::network::detail
{

template <typename Value> struct InterruptedOperationResult
{
    Value value;
    int error_code;
};

/// Repeats an operation while its error is classified as an interruption.
///
/// The operation, error provider, and retry predicate are injected so the retry
/// control flow can be tested deterministically without signals or timing.
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

/// Reports whether a failed native accept should be attempted again.
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
