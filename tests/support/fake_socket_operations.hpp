#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "sparenode/network/detail/connection_io.hpp"

namespace sparenode::test
{

/// Controls immediate native-transfer results while exercising production I/O policy.
class FakeSocketOperations final : public network::detail::SocketOperations
{
  public:
    [[nodiscard]] std::ptrdiff_t receive(network::detail::NativeSocket,
                                         const std::span<std::byte> buffer) noexcept override
    {
        ++receive_calls_;
        return bounded_result(receive_result_, buffer.size());
    }

    [[nodiscard]] std::ptrdiff_t send(network::detail::NativeSocket,
                                      const std::span<const std::byte> buffer) noexcept override
    {
        ++send_calls_;
        sent_bytes_.clear();
        const auto transferred = bounded_result(send_result_, buffer.size());
        if (transferred > 0)
        {
            const auto sent = buffer.first(static_cast<std::size_t>(transferred));
            sent_bytes_.assign(sent.begin(), sent.end());
        }
        return transferred;
    }

    [[nodiscard]] int last_error() const noexcept override
    {
        return error_code_;
    }

    void set_receive_result(const std::ptrdiff_t result) noexcept
    {
        receive_result_ = result;
    }

    void set_send_result(const std::ptrdiff_t result) noexcept
    {
        send_result_ = result;
    }

    void set_error_code(const int error_code) noexcept
    {
        error_code_ = error_code;
    }

    [[nodiscard]] std::size_t receive_calls() const noexcept
    {
        return receive_calls_;
    }

    [[nodiscard]] std::size_t send_calls() const noexcept
    {
        return send_calls_;
    }

    [[nodiscard]] std::span<const std::byte> sent_bytes() const noexcept
    {
        return sent_bytes_;
    }

  private:
    /// Clamps successful native-style results to the supplied buffer boundary.
    [[nodiscard]] static std::ptrdiff_t bounded_result(const std::ptrdiff_t result,
                                                       const std::size_t buffer_size) noexcept
    {
        if (result <= 0)
        {
            return result;
        }

        return static_cast<std::ptrdiff_t>(
            (std::min)(static_cast<std::size_t>(result), buffer_size));
    }

    std::ptrdiff_t receive_result_{0};
    std::ptrdiff_t send_result_{0};
    int error_code_{0};
    std::size_t receive_calls_{0};
    std::size_t send_calls_{0};
    std::vector<std::byte> sent_bytes_;
};

} // namespace sparenode::test
