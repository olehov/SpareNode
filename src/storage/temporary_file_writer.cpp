#include "sparenode/storage/temporary_file_writer.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

#include "sparenode/foundation/concurrency/interruption.hpp"

namespace sparenode::storage
{
namespace
{

using FileOperationResult = Result<void, TemporaryFileError>;

/// 16 KiB matches the bounded request decode chunk without retaining request memory.
constexpr std::size_t isolated_payload_bytes = std::size_t{16} * 1024;

/// @brief Converts worker creation failures into the storage resource category.
[[nodiscard]] FileOperationResult worker_unavailable(const int native_code = 0)
{
    return unexpected(
        TemporaryFileError{TemporaryFileErrorCode::resource_allocation_failed, native_code});
}

/// @brief Maps shared interruption policy into the temporary-file error domain.
[[nodiscard]] std::optional<TemporaryFileError>
interruption(const TemporaryFileIoOptions &options) noexcept
{
    switch (concurrency::interruption_status(options.stop_token, options.deadline))
    {
    case concurrency::InterruptionStatus::cancelled:
        return TemporaryFileError{TemporaryFileErrorCode::cancelled, 0};
    case concurrency::InterruptionStatus::deadline_exceeded:
        return TemporaryFileError{TemporaryFileErrorCode::deadline_exceeded, 0};
    case concurrency::InterruptionStatus::none:
        return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Bounds active native file operations, including those orphaned by timeouts.
class FileWorkerSlots
{
  public:
    struct Ticket
    {
        bool orphaned{}; ///< A request stopped waiting during this native operation.
        bool released{}; ///< Prevents accounting the same worker twice.
    };

    /// @brief Waits for capacity while respecting the caller's active policy.
    [[nodiscard]] Result<std::shared_ptr<Ticket>, TemporaryFileError>
    acquire(const TemporaryFileIoOptions &options)
    {
        auto ticket = std::make_shared<Ticket>();
        std::unique_lock lock(mutex_);
        const auto available = [this]
        { return active_ < maximum_workers || orphaned_ == maximum_workers; };
        if (options.deadline.has_value())
        {
            static_cast<void>(available_.wait_until(lock, options.stop_token,
                                                    options.deadline.value(), available));
        }
        else
        {
            static_cast<void>(available_.wait(lock, options.stop_token, available));
        }
        if (const auto stopped = interruption(options); stopped)
        {
            return unexpected(stopped.value());
        }
        if (orphaned_ == maximum_workers)
        {
            return unexpected(
                TemporaryFileError{TemporaryFileErrorCode::resource_allocation_failed, 0});
        }
        ++active_;
        return ticket;
    }

    /// @brief Marks a detached worker without freeing its occupied capacity.
    void orphan(const std::shared_ptr<Ticket> &ticket) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!ticket->released && !ticket->orphaned)
        {
            ticket->orphaned = true;
            ++orphaned_;
            available_.notify_all();
        }
    }

    /// @brief Returns capacity after native I/O and deferred cleanup finish.
    void release(const std::shared_ptr<Ticket> &ticket) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!ticket->released)
        {
            ticket->released = true;
            --active_;
            if (ticket->orphaned)
            {
                --orphaned_;
            }
            available_.notify_all();
        }
    }

  private:
    /// 32 isolates storage stalls while preventing unbounded detached workers.
    static constexpr std::size_t maximum_workers = 32;
    std::mutex mutex_;                      ///< Protects worker and orphan counts.
    std::condition_variable_any available_; ///< Wakes queued callers after release.
    std::size_t active_{};                  ///< Native operations currently owning a slot.
    std::size_t orphaned_{};                ///< Active operations detached by a caller.
};

/// @brief Keeps accounting alive until process exit, including for detached workers.
[[nodiscard]] FileWorkerSlots &file_worker_slots()
{
    static auto *slots = new FileWorkerSlots();
    return *slots;
}

} // namespace

/// @brief Reuses one native I/O thread and fixed payload buffer for an artifact.
struct TemporaryFileWriter::FileWorker
{
    /// @brief Selects the native operation queued on the isolated worker.
    enum class Operation : std::uint8_t
    {
        write,   ///< Append the currently queued payload bytes.
        complete ///< Flush and close the file after the final bytes.
    };

    /// @brief Keeps artifact ownership and queued bytes independent of the caller.
    struct State
    {
        /// @brief Retains the artifact until the worker completes deferred cleanup.
        /// @param[in] owned_file Artifact shared with the writer until caller detachment.
        explicit State(std::shared_ptr<TemporaryFile> owned_file) : file(std::move(owned_file))
        {
        }

        std::mutex mutex;                                ///< Protects work and result publication.
        std::condition_variable_any changed;             ///< Wakes the file and calling workers.
        std::shared_ptr<TemporaryFile> file;             ///< Owns cleanup after caller detachment.
        std::shared_ptr<FileWorkerSlots::Ticket> ticket; ///< Current operation capacity.
        std::array<std::byte, isolated_payload_bytes> buffer{}; ///< Copied write payload.
        TemporaryFileIoOptions options{};          ///< Policy for the queued operation.
        std::optional<FileOperationResult> result; ///< Result of the latest operation.
        std::size_t bytes{};                       ///< Current payload length.
        std::uint64_t sequence{};                  ///< Latest queued operation identifier.
        std::uint64_t completed{};                 ///< Latest finished operation identifier.
        Operation operation{Operation::write};     ///< Kind of queued operation.
        bool pending{};                            ///< One operation is queued or running.
        bool stopping{};                           ///< Exit after the current operation.
    };

    /// @brief Owns one persistent I/O thread's shared operation state.
    /// @param[in] worker_state Queue, synchronization, and artifact ownership.
    explicit FileWorker(std::shared_ptr<State> worker_state) : state(std::move(worker_state))
    {
    }

    ~FileWorker()
    {
        if (thread.joinable())
        {
            abandon();
        }
    }

    /// @brief Processes successive writes and completion without replacing the thread.
    /// @param[in] state Shared operation queue and artifact ownership.
    static void work(const std::shared_ptr<State> &state) noexcept
    {
        while (true)
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&state] { return state->pending || state->stopping; });
            if (!state->pending)
            {
                break;
            }
            const auto operation = state->operation;
            const auto bytes = state->bytes;
            const auto options = state->options;
            const auto sequence = state->sequence;
            lock.unlock();

            FileOperationResult result;
            try
            {
                result = operation == Operation::write
                             ? state->file->write(std::span(state->buffer).first(bytes), options)
                             : state->file->complete(options);
            }
            catch (...)
            {
                result = worker_unavailable();
            }

            lock.lock();
            state->result.emplace(result);
            state->completed = sequence;
            state->pending = false;
            const auto ticket = std::move(state->ticket);
            const auto stopping = state->stopping;
            lock.unlock();
            if (stopping)
            {
                state->file.reset();
            }
            file_worker_slots().release(ticket);
            state->changed.notify_all();
        }
        state->file.reset();
    }

    /// @brief Queues one operation and waits only until completion or policy interruption.
    /// @param[in] operation Append or durable completion operation to perform.
    /// @param[in] payload Copied bytes for a write, or an empty span for completion.
    /// @param[in] options Cancellation and deadline governing capacity and waiting.
    /// @return Native operation result or the first observed interruption.
    [[nodiscard]] FileOperationResult run(const Operation operation,
                                          const std::span<const std::byte> payload,
                                          const TemporaryFileIoOptions &options)
    {
        std::shared_ptr<FileWorkerSlots::Ticket> ticket;
        try
        {
            auto acquired = file_worker_slots().acquire(options);
            if (!acquired)
            {
                return unexpected(acquired.error());
            }
            ticket = std::move(acquired).value();
        }
        catch (const std::bad_alloc &)
        {
            return worker_unavailable();
        }

        std::unique_lock lock(state->mutex);
        if (state->stopping || state->pending || payload.size() > state->buffer.size())
        {
            file_worker_slots().release(ticket);
            return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
        }
        std::copy(payload.begin(), payload.end(), state->buffer.begin());
        state->bytes = payload.size();
        state->operation = operation;
        state->options = options;
        state->ticket = ticket;
        state->result.reset();
        const auto sequence = ++state->sequence;
        state->pending = true;
        lock.unlock();
        state->changed.notify_all();
        lock.lock();

        const auto finished = [&] { return state->completed == sequence; };
        if (options.deadline.has_value())
        {
            static_cast<void>(state->changed.wait_until(lock, options.stop_token,
                                                        options.deadline.value(), finished));
        }
        else
        {
            static_cast<void>(state->changed.wait(lock, options.stop_token, finished));
        }
        if (const auto stopped = interruption(options); stopped)
        {
            return unexpected(stopped.value());
        }
        if (!finished())
        {
            return worker_unavailable();
        }
        return state->result.value_or(worker_unavailable());
    }

    /// @brief Detaches a stalled operation while retaining cleanup and capacity accounting.
    void abandon() noexcept
    {
        std::shared_ptr<FileWorkerSlots::Ticket> ticket;
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
            ticket = state->ticket;
        }
        state->changed.notify_all();
        if (ticket)
        {
            file_worker_slots().orphan(ticket);
        }
        try
        {
            thread.detach();
        }
        catch (const std::system_error &)
        {
            thread.join();
        }
    }

    /// @brief Stops and joins the worker after successful durable completion.
    void finish() noexcept
    {
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
        }
        state->changed.notify_all();
        thread.join();
    }

    std::shared_ptr<State> state; ///< Owned until joined or detached completion.
    std::thread thread;           ///< One thread per active artifact.
};

TemporaryFileWriter::TemporaryFileWriter(std::shared_ptr<TemporaryFile> file) noexcept
    : file_(std::move(file))
{
}

TemporaryFileWriter::~TemporaryFileWriter()
{
    if (worker_)
    {
        file_.reset();
        worker_->abandon();
    }
}

Result<std::unique_ptr<TemporaryFileWriter>, TemporaryFileError>
TemporaryFileWriter::create(const TemporaryFileFactory &factory)
{
    try
    {
        std::shared_ptr<TemporaryFile> file;
        if (factory)
        {
            auto created = factory();
            if (!created)
            {
                return unexpected(created.error());
            }
            file = std::move(created).value();
        }
        else
        {
            auto created = TemporaryFile::create();
            if (!created)
            {
                return unexpected(created.error());
            }
            file = std::make_shared<TemporaryFile>(std::move(created).value());
        }
        if (!file)
        {
            return unexpected(
                TemporaryFileError{TemporaryFileErrorCode::resource_allocation_failed, 0});
        }
        return std::unique_ptr<TemporaryFileWriter>(new TemporaryFileWriter(std::move(file)));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            TemporaryFileError{TemporaryFileErrorCode::resource_allocation_failed, 0});
    }
}

Result<void, TemporaryFileError>
TemporaryFileWriter::ensure_worker(const TemporaryFileIoOptions &options)
{
    if (worker_ || (!options.deadline.has_value() && !options.stop_token.stop_possible()))
    {
        return {};
    }
    try
    {
        auto state = std::make_shared<FileWorker::State>(file_);
        auto worker = std::make_unique<FileWorker>(state);
        worker->thread = std::thread([state] { FileWorker::work(state); });
        worker_ = std::move(worker);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return worker_unavailable();
    }
    catch (const std::system_error &error)
    {
        return worker_unavailable(error.code().value());
    }
}

Result<void, TemporaryFileError> TemporaryFileWriter::write(const std::span<const std::byte> bytes,
                                                            const TemporaryFileIoOptions &options)
{
    if (completed_ || !file_)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
    }
    if (auto ready = ensure_worker(options); !ready)
    {
        return ready;
    }
    if (!worker_)
    {
        return file_->write(bytes, options);
    }
    if (bytes.empty())
    {
        return worker_->run(FileWorker::Operation::write, {}, options);
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = (std::min)(bytes.size() - offset, isolated_payload_bytes);
        if (auto written =
                worker_->run(FileWorker::Operation::write, bytes.subspan(offset, count), options);
            !written)
        {
            return written;
        }
        offset += count;
    }
    return {};
}

Result<void, TemporaryFileError>
TemporaryFileWriter::complete(const TemporaryFileIoOptions &options)
{
    if (completed_ || !file_)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
    }
    if (auto ready = ensure_worker(options); !ready)
    {
        return ready;
    }
    auto result = worker_ ? worker_->run(FileWorker::Operation::complete, {}, options)
                          : file_->complete(options);
    if (!result)
    {
        return result;
    }
    if (worker_)
    {
        worker_->finish();
        worker_.reset();
    }
    completed_ = true;
    return {};
}

std::shared_ptr<const TemporaryFile> TemporaryFileWriter::artifact() const noexcept
{
    return completed_ ? file_ : nullptr;
}

} // namespace sparenode::storage
