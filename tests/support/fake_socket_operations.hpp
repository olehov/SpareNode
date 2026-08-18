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
                                         const std::span<std::byte>) noexcept override
    {
        ++receive_calls_;
        return receive_result_;
    }

    [[nodiscard]] std::ptrdiff_t send(network::detail::NativeSocket,
                                      const std::span<const std::byte> buffer) noexcept override
    {
        ++send_calls_;
        if (send_result_ > 0)
        {
            const auto transferred =
                (std::min)(static_cast<std::size_t>(send_result_), buffer.size());
            const auto sent = buffer.first(transferred);
            sent_bytes_.assign(sent.begin(), sent.end());
        }
        return send_result_;
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
    std::ptrdiff_t receive_result_{0};
    std::ptrdiff_t send_result_{0};
    int error_code_{0};
    std::size_t receive_calls_{0};
    std::size_t send_calls_{0};
    std::vector<std::byte> sent_bytes_;
};

} // namespace sparenode::test
