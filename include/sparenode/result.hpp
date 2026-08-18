#pragma once

#include <concepts>
#include <utility>
#include <variant>

namespace sparenode
{

/// @brief Tags an error so Result can distinguish failure from a successful value.
/// @tparam Error Error value stored by the failed result.
template <typename Error> struct Unexpected
{
    /// @brief Error value to transfer into a Result.
    Error error;
};

/// @brief Creates the failure alternative of a Result.
/// @tparam Error Error value stored by the failed result.
/// @param[in] error Error to move into the wrapper.
/// @return A tagged error that selects the failure alternative of Result.
template <typename Error> [[nodiscard]] Unexpected<Error> unexpected(Error error)
{
    return Unexpected<Error>{std::move(error)};
}

/// @brief Stores either a successful Value or an Error without throwing for normal failures.
///
/// This project-local type provides the subset of std::expected needed by SpareNode.
/// It keeps the public API portable to C++23 toolchains whose standard library does
/// not yet provide std::expected (notably some Clang/libstdc++ combinations).
///
/// @tparam Value Successful value type.
/// @tparam Error Failure value type.
template <typename Value, typename Error> class Result
{
  public:
    /// @brief Creates a successful result by copying a copyable value.
    /// @param[in] value Value to copy into the result.
    // Implicit value conversion keeps successful return statements concise.
    // cppcheck-suppress noExplicitConstructor
    Result(const Value &value)
        requires std::copy_constructible<Value>
        : storage_(std::in_place_index<0>, value)
    {
    }

    /// @brief Creates a successful result by moving a value into internal storage.
    /// @param[in] value Value to move into the result.
    // Implicit value conversion keeps successful return statements concise.
    // cppcheck-suppress noExplicitConstructor
    Result(Value &&value) : storage_(std::in_place_index<0>, std::move(value))
    {
    }

    /// @brief Creates a failed result from an explicitly tagged error.
    /// @param[in] failure Tagged error to move into the result.
    // Unexpected is already an explicit failure tag, so another cast adds no safety.
    // cppcheck-suppress noExplicitConstructor
    Result(Unexpected<Error> failure) : storage_(std::in_place_index<1>, std::move(failure.error))
    {
    }

    /// @brief Reports whether the result contains a successful value.
    /// @return `true` for success, or `false` for failure.
    [[nodiscard]] bool has_value() const noexcept
    {
        return storage_.index() == 0;
    }

    /// @brief Enables concise checks such as `if (result)`.
    /// @return `true` for success, or `false` for failure.
    explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// @brief Accesses the successful value through a mutable lvalue result.
    /// @return A mutable reference to the stored value.
    /// @pre has_value() is `true`.
    /// @throws std::bad_variant_access If the result contains an error.
    [[nodiscard]] Value &value() &
    {
        return std::get<0>(storage_);
    }

    /// @brief Accesses the successful value through a const lvalue result.
    /// @return A const reference to the stored value.
    /// @pre has_value() is `true`.
    /// @throws std::bad_variant_access If the result contains an error.
    [[nodiscard]] const Value &value() const &
    {
        return std::get<0>(storage_);
    }

    /// @brief Moves the successful value out of a temporary result.
    /// @return An rvalue reference to the stored value.
    /// @pre has_value() is `true`.
    /// @throws std::bad_variant_access If the result contains an error.
    [[nodiscard]] Value &&value() &&
    {
        return std::get<0>(std::move(storage_));
    }

    /// @brief Accesses the failure through a mutable lvalue result.
    /// @return A mutable reference to the stored error.
    /// @pre has_value() is `false`.
    /// @throws std::bad_variant_access If the result contains a value.
    [[nodiscard]] Error &error() &
    {
        return std::get<1>(storage_);
    }

    /// @brief Accesses the failure through a const result.
    /// @return A const reference to the stored error.
    /// @pre has_value() is `false`.
    /// @throws std::bad_variant_access If the result contains a value.
    [[nodiscard]] const Error &error() const &
    {
        return std::get<1>(storage_);
    }

    /// @brief Provides pointer-style access to the successful value.
    /// @return A pointer to the stored value.
    /// @pre has_value() is `true`.
    /// @throws std::bad_variant_access If the result contains an error.
    [[nodiscard]] Value *operator->()
    {
        return &value();
    }

    /// @brief Provides const pointer-style access to the successful value.
    /// @return A const pointer to the stored value.
    /// @pre has_value() is `true`.
    /// @throws std::bad_variant_access If the result contains an error.
    [[nodiscard]] const Value *operator->() const
    {
        return &value();
    }

  private:
    /// @brief Storage whose zero index is success and whose one index is failure.
    std::variant<Value, Error> storage_;
};

/// @brief Result specialization for operations that succeed without returning a value.
/// @tparam Error Failure value type.
template <typename Error> class Result<void, Error>
{
  public:
    /// @brief Creates a successful result for an operation with no return value.
    Result() : storage_(std::in_place_index<0>)
    {
    }

    /// @brief Creates a failed void result from an explicitly tagged error.
    /// @param[in] failure Tagged error to move into the result.
    // Unexpected is already an explicit failure tag, so another cast adds no safety.
    // cppcheck-suppress noExplicitConstructor
    Result(Unexpected<Error> failure) : storage_(std::in_place_index<1>, std::move(failure.error))
    {
    }

    /// @brief Reports whether the void operation succeeded.
    /// @return `true` for success, or `false` for failure.
    [[nodiscard]] bool has_value() const noexcept
    {
        return storage_.index() == 0;
    }

    /// @brief Enables concise success checks such as `if (result)`.
    /// @return `true` for success, or `false` for failure.
    explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// @brief Accesses the failure through a mutable lvalue result.
    /// @return A mutable reference to the stored error.
    /// @pre has_value() is `false`.
    /// @throws std::bad_variant_access If the result represents success.
    [[nodiscard]] Error &error() &
    {
        return std::get<1>(storage_);
    }

    /// @brief Accesses the failure through a const result.
    /// @return A const reference to the stored error.
    /// @pre has_value() is `false`.
    /// @throws std::bad_variant_access If the result represents success.
    [[nodiscard]] const Error &error() const &
    {
        return std::get<1>(storage_);
    }

  private:
    /// @brief Storage using monostate for success so Error need not be default-constructible.
    std::variant<std::monostate, Error> storage_;
};

} // namespace sparenode
