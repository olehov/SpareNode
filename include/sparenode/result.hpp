#pragma once

#include <concepts>
#include <utility>
#include <variant>

namespace sparenode
{

/// Tags an error so Result can distinguish failure from a successful value.
template <typename Error> struct Unexpected
{
    Error error;
};

/// Creates the failure alternative of a Result.
template <typename Error> [[nodiscard]] Unexpected<Error> unexpected(Error error)
{
    return Unexpected<Error>{std::move(error)};
}

/// Stores either a successful Value or an Error without throwing for normal failures.
///
/// This project-local type provides the subset of std::expected needed by SpareNode.
/// It keeps the public API portable to C++23 toolchains whose standard library does
/// not yet provide std::expected (notably some Clang/libstdc++ combinations).
template <typename Value, typename Error> class Result
{
  public:
    /// Creates a successful result by copying a copyable value.
    Result(const Value &value)
        requires std::copy_constructible<Value>
        : storage_(std::in_place_index<0>, value)
    {
    }

    /// Creates a successful result by moving a value into internal storage.
    Result(Value &&value) : storage_(std::in_place_index<0>, std::move(value))
    {
    }

    /// Creates a failed result from an explicitly tagged error.
    Result(Unexpected<Error> failure) : storage_(std::in_place_index<1>, std::move(failure.error))
    {
    }

    /// Reports whether the result contains a successful value.
    [[nodiscard]] bool has_value() const noexcept
    {
        return storage_.index() == 0;
    }

    /// Enables concise checks such as `if (result)`.
    explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// Accesses the successful value. The caller must check has_value() first.
    [[nodiscard]] Value &value() &
    {
        return std::get<0>(storage_);
    }

    /// Accesses the successful value through a const lvalue result.
    [[nodiscard]] const Value &value() const &
    {
        return std::get<0>(storage_);
    }

    /// Moves the successful value out of a temporary result.
    [[nodiscard]] Value &&value() &&
    {
        return std::get<0>(std::move(storage_));
    }

    /// Accesses the failure. The caller must check that no value is present first.
    [[nodiscard]] Error &error() &
    {
        return std::get<1>(storage_);
    }

    /// Accesses the failure through a const result.
    [[nodiscard]] const Error &error() const &
    {
        return std::get<1>(storage_);
    }

    /// Provides pointer-style access to the successful value.
    [[nodiscard]] Value *operator->()
    {
        return &value();
    }

    /// Provides const pointer-style access to the successful value.
    [[nodiscard]] const Value *operator->() const
    {
        return &value();
    }

  private:
    // Index zero is always success; index one is always failure.
    std::variant<Value, Error> storage_;
};

/// Result specialization for operations that succeed without returning a value.
template <typename Error> class Result<void, Error>
{
  public:
    /// Creates a successful result for an operation with no return value.
    Result() : storage_(std::in_place_index<0>)
    {
    }

    /// Creates a failed void result from an explicitly tagged error.
    Result(Unexpected<Error> failure) : storage_(std::in_place_index<1>, std::move(failure.error))
    {
    }

    /// Reports whether the void operation succeeded.
    [[nodiscard]] bool has_value() const noexcept
    {
        return storage_.index() == 0;
    }

    /// Enables concise success checks such as `if (result)`.
    explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// Accesses the failure after the caller has checked that the operation failed.
    [[nodiscard]] Error &error() &
    {
        return std::get<1>(storage_);
    }

    /// Accesses the failure through a const result.
    [[nodiscard]] const Error &error() const &
    {
        return std::get<1>(storage_);
    }

  private:
    // monostate represents success without requiring Error to be default-constructible.
    std::variant<std::monostate, Error> storage_;
};

} // namespace sparenode
