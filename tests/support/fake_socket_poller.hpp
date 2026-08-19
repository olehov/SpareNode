#pragma once

#include <limits>
#include <optional>
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
        for (auto &entry : entries)
        {
            entry.readable = false;
            entry.writable = false;
            entry.error = false;
            entry.hangup = false;
            entry.invalid = false;
        }
        readable_index_ = (std::numeric_limits<std::size_t>::max)();
        writable_index_ = (std::numeric_limits<std::size_t>::max)();
        error_index_ = (std::numeric_limits<std::size_t>::max)();
        hangup_index_ = (std::numeric_limits<std::size_t>::max)();
        invalid_index_ = (std::numeric_limits<std::size_t>::max)();
        error_.reset();
        operation_ = operation;
        entry_count_ = entries.size();
        if (!entries.empty())
        {
            watches_readable_ = entries.front().watch_readable;
            watches_writable_ = entries.front().watch_writable;
        }
        entered_.release();
        resume_.acquire();
        if (error_.has_value())
        {
            return unexpected(error_.value());
        }
        if (readable_index_ < entries.size())
        {
            entries[readable_index_].readable = true;
        }
        if (writable_index_ < entries.size())
        {
            entries[writable_index_].writable = true;
        }
        if (error_index_ < entries.size())
        {
            entries[error_index_].error = true;
        }
        if (hangup_index_ < entries.size())
        {
            // The waiting production code observes this output after the fake returns.
            // cppcheck-suppress unreadVariable
            entries[hangup_index_].hangup = true;
        }
        if (invalid_index_ < entries.size())
        {
            // The waiting production code observes this output after the fake returns.
            // cppcheck-suppress unreadVariable
            entries[invalid_index_].invalid = true;
        }
        return {};
    }

    /// Blocks the test until production code reaches the poll abstraction.
    void wait_until_entered()
    {
        entered_.acquire();
    }

    /// Stages readable state without completing the blocked fake wait.
    /// @param[in] index Entry that becomes readable on the next completion.
    void stage_readable(const std::size_t index)
    {
        readable_index_ = index;
    }

    /// Returns from the fake wait with one socket reported as readable.
    void complete_with_readable(const std::size_t index)
    {
        readable_index_ = index;
        resume_.release();
    }

    /// Returns from the fake wait with one socket reported as writable.
    void complete_with_writable(const std::size_t index)
    {
        writable_index_ = index;
        resume_.release();
    }

    /// Returns from the fake wait with one socket reporting an error event.
    void complete_with_socket_error(const std::size_t index)
    {
        error_index_ = index;
        resume_.release();
    }

    /// Returns from the fake wait with one socket reporting a hangup event.
    void complete_with_hangup(const std::size_t index)
    {
        hangup_index_ = index;
        resume_.release();
    }

    /// Returns from the fake wait with one socket reported as invalid.
    void complete_with_invalid(const std::size_t index)
    {
        invalid_index_ = index;
        resume_.release();
    }

    /// Returns from the fake wait with the supplied polling failure.
    void complete_with_error(const network::NetworkError error)
    {
        error_ = error;
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

    /// Reports whether the operation socket requested readable readiness.
    [[nodiscard]] bool watches_readable() const noexcept
    {
        return watches_readable_;
    }

    /// Reports whether the operation socket requested writable readiness.
    [[nodiscard]] bool watches_writable() const noexcept
    {
        return watches_writable_;
    }

  private:
    std::binary_semaphore entered_{0};
    std::binary_semaphore resume_{0};
    network::NetworkOperation operation_{};
    std::size_t entry_count_{0};
    bool watches_readable_{false};
    bool watches_writable_{false};
    std::size_t readable_index_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t writable_index_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t error_index_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t hangup_index_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t invalid_index_{(std::numeric_limits<std::size_t>::max)()};
    std::optional<network::NetworkError> error_;
};

} // namespace sparenode::test
