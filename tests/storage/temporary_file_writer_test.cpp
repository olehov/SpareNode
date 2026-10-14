#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

#include "sparenode/storage/temporary_file_writer.hpp"

namespace
{

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct BlockingState
{
    std::mutex mutex;
    std::condition_variable changed;
    bool started{};
    bool released{};
    bool destroyed{};
};

/// @brief Waits for one worker state transition without assuming scheduler timing.
void require_state_flag(const std::shared_ptr<BlockingState> &state,
                        const bool BlockingState::*flag)
{
    std::unique_lock lock(state->mutex);
    REQUIRE(state->changed.wait_for(lock, std::chrono::seconds{2},
                                    [&state, flag] { return state.get()->*flag; }));
}

class BlockingTemporaryFile final : public sparenode::storage::TemporaryFile
{
  public:
    explicit BlockingTemporaryFile(std::shared_ptr<BlockingState> state) : state_(std::move(state))
    {
    }

    ~BlockingTemporaryFile() override
    {
        {
            std::lock_guard lock(state_->mutex);
            state_->destroyed = true;
        }
        state_->changed.notify_all();
    }

    [[nodiscard]] sparenode::Result<void, sparenode::storage::TemporaryFileError>
    write(std::span<const std::byte>, const sparenode::storage::TemporaryFileIoOptions &) override
    {
        std::unique_lock lock(state_->mutex);
        state_->started = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [this] { return state_->released; });
        return {};
    }

  private:
    std::shared_ptr<BlockingState> state_;
};

} // namespace

TEST_CASE("Temporary file writer publishes a completed artifact", "[storage][temporary-file]")
{
    auto writer = sparenode::storage::TemporaryFileWriter::create();
    REQUIRE(writer.has_value());
    REQUIRE(writer.value()->write(bytes("bounded artifact")).has_value());
    REQUIRE(writer.value()->complete().has_value());
    const auto artifact = writer.value()->artifact();
    REQUIRE(artifact);
    CHECK(artifact->completed());
    CHECK(artifact->size() == 16);
    CHECK(read_file(artifact->path()) == "bounded artifact");
}

TEST_CASE("Temporary file writer preserves injected creation failures", "[storage][temporary-file]")
{
    using sparenode::storage::TemporaryFile;
    using sparenode::storage::TemporaryFileError;
    using sparenode::storage::TemporaryFileErrorCode;

    auto writer = sparenode::storage::TemporaryFileWriter::create(
        []() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            return sparenode::unexpected(
                TemporaryFileError{TemporaryFileErrorCode::create_failed, 7});
        });
    REQUIRE_FALSE(writer.has_value());
    CHECK(writer.error().code == TemporaryFileErrorCode::create_failed);
    CHECK(writer.error().native_code == 7);
}

TEST_CASE("Temporary file writer chunks policy-bound payloads without truncation",
          "[storage][temporary-file]")
{
    std::string payload(std::size_t{20} * 1024, 'x');
    std::stop_source stop;
    auto writer = sparenode::storage::TemporaryFileWriter::create();
    REQUIRE(writer.has_value());
    REQUIRE(writer.value()->write(bytes(payload), {.stop_token = stop.get_token()}).has_value());
    REQUIRE(writer.value()->complete({.stop_token = stop.get_token()}).has_value());
    const auto artifact = writer.value()->artifact();
    REQUIRE(artifact);
    CHECK(artifact->size() == payload.size());
    CHECK(read_file(artifact->path()) == payload);
}

TEST_CASE("Timed out storage I/O retains cleanup ownership until the native call returns",
          "[storage][temporary-file][timeout]")
{
    using sparenode::storage::TemporaryFile;
    using sparenode::storage::TemporaryFileError;
    using sparenode::storage::TemporaryFileErrorCode;

    auto state = std::make_shared<BlockingState>();
    auto writer = sparenode::storage::TemporaryFileWriter::create(
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<BlockingTemporaryFile>(state));
        });
    REQUIRE(writer.has_value());

    const auto written =
        writer.value()->write(bytes("blocked"), {.deadline = std::chrono::steady_clock::now() +
                                                             std::chrono::milliseconds{50}});
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == TemporaryFileErrorCode::deadline_exceeded);

    require_state_flag(state, &BlockingState::started);
    writer.value().reset();

    {
        std::lock_guard lock(state->mutex);
        CHECK_FALSE(state->destroyed);
        state->released = true;
    }
    state->changed.notify_all();

    require_state_flag(state, &BlockingState::destroyed);
}
