#pragma once

#include <limits>
#include <semaphore>
#include <span>

#include "sparenode/network/detail/socket_poller.hpp"

namespace sparenode::test
{

/// A controllable poller used to test wait algorithms without native timing races.
class FakeSocketPoller final : public network::detail::SocketPoller
{
  public:
    /// Records that polling started, then waits until the test releases it.
    [[nodiscard]] Result<void, network::NetworkError>
    wait(std::span<network::detail::SocketPollEntry> entries,
         network::NetworkOperation operation) override
    {
        operation_ = operation;
        entry_count_ = entries.size();
        entered_.release();
        resume_.acquire();
        if (readable_index_ < entries.size())
        {
            entries[readable_index_].readable = true;
        }
        return {};
    }

    /// Blocks the test until production code reaches the poll abstraction.
    void wait_until_entered()
    {
        entered_.acquire();
    }

    /// Returns from the fake wait with one socket reported as readable.
    void complete_with_readable(const std::size_t index)
    {
        readable_index_ = index;
        resume_.release();
    }

    /// Returns the operation supplied by the code under test.
    [[nodiscard]] network::NetworkOperation operation() const noexcept
    {
        return operation_;
    }

    /// Returns the number of sockets supplied to the fake poll operation.
    [[nodiscard]] std::size_t entry_count() const noexcept
    {
        return entry_count_;
    }

  private:
    std::binary_semaphore entered_{0};
    std::binary_semaphore resume_{0};
    network::NetworkOperation operation_{};
    std::size_t entry_count_{0};
    std::size_t readable_index_{(std::numeric_limits<std::size_t>::max)()};
};

} // namespace sparenode::test
