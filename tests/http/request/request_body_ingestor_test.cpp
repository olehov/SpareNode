#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

#include "sparenode/http/request/request_body_ingestor.hpp"

namespace
{
/// @brief Produces one controlled native-I/O failure for ingestion mapping tests.
class FailingTemporaryFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    explicit FailingTemporaryFile(const sparenode::filesystem::TemporaryFileErrorCode code)
        : code_(code)
    {
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(std::span<const std::byte>,
          const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        if (code_ == sparenode::filesystem::TemporaryFileErrorCode::write_failed)
        {
            return sparenode::unexpected(
                sparenode::filesystem::TemporaryFileError{code_, native_error});
        }
        return {};
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete(const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        return sparenode::unexpected(
            sparenode::filesystem::TemporaryFileError{code_, native_error});
    }

  private:
    static constexpr int native_error = 1234;
    sparenode::filesystem::TemporaryFileErrorCode code_;
};

/// @brief Shares synchronization and destruction state with blocked-write tests.
struct BlockedWriteState
{
    std::promise<void> started;       ///< Fulfilled after write receives active policy.
    std::atomic_bool destroyed{};     ///< Set when ingestion releases the failed artifact.
    std::mutex mutex;                 ///< Protects the cancellable test wait.
    std::condition_variable_any wake; ///< Wakes the wait at cancellation or deadline.
};

/// @brief Emulates a blocked sink that honors the supplied stop token and deadline.
class BlockingTemporaryFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    explicit BlockingTemporaryFile(std::shared_ptr<BlockedWriteState> state)
        : state_(std::move(state))
    {
    }

    ~BlockingTemporaryFile() override
    {
        state_->destroyed.store(true, std::memory_order_release);
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(std::span<const std::byte>,
          const sparenode::filesystem::TemporaryFileIoOptions &options) override
    {
        state_->started.set_value();
        std::unique_lock lock(state_->mutex);
        if (options.deadline.has_value())
        {
            static_cast<void>(state_->wake.wait_until(
                lock, options.stop_token, options.deadline.value(), [] { return false; }));
        }
        else
        {
            static_cast<void>(state_->wake.wait(lock, options.stop_token, [] { return false; }));
        }
        return sparenode::unexpected(sparenode::filesystem::TemporaryFileError{
            options.stop_token.stop_requested()
                ? sparenode::filesystem::TemporaryFileErrorCode::cancelled
                : sparenode::filesystem::TemporaryFileErrorCode::deadline_exceeded,
            0});
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete(const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        return {};
    }

  private:
    std::shared_ptr<BlockedWriteState> state_;
};

/// @brief Holds a native-call simulation that intentionally ignores request policy.
struct StalledWriteState
{
    std::promise<void> started;     ///< Signals entry into the uncooperative write.
    std::promise<void> destroyed;   ///< Signals eventual artifact cleanup.
    std::mutex mutex;               ///< Protects the test-controlled release gate.
    std::condition_variable resume; ///< Wakes the stalled file worker.
    bool released{};                ///< Allows the simulated native call to return.
    std::filesystem::path path;     ///< Native artifact path when the test uses a real file.
};

/// @brief Models a filesystem driver that cannot cancel an in-progress write.
class StalledTemporaryFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    explicit StalledTemporaryFile(std::shared_ptr<StalledWriteState> state)
        : state_(std::move(state))
    {
    }

    ~StalledTemporaryFile() override
    {
        state_->destroyed.set_value();
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(std::span<const std::byte>,
          const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        state_->started.set_value();
        std::unique_lock lock(state_->mutex);
        state_->resume.wait(lock, [this] { return state_->released; });
        return {};
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete(const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        return {};
    }

  private:
    std::shared_ptr<StalledWriteState> state_;
};

/// @brief Models a filesystem driver that cannot cancel an in-progress flush.
class StalledCompletionFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    explicit StalledCompletionFile(std::shared_ptr<StalledWriteState> state)
        : state_(std::move(state))
    {
    }

    ~StalledCompletionFile() override
    {
        state_->destroyed.set_value();
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(std::span<const std::byte>,
          const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        return {};
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete(const sparenode::filesystem::TemporaryFileIoOptions &) override
    {
        state_->started.set_value();
        std::unique_lock lock(state_->mutex);
        state_->resume.wait(lock, [this] { return state_->released; });
        return {};
    }

  private:
    std::shared_ptr<StalledWriteState> state_;
};

/// @brief Holds a real temporary file across a simulated uninterruptible write.
class StalledNativeBackedFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    StalledNativeBackedFile(std::shared_ptr<StalledWriteState> state,
                            sparenode::filesystem::TemporaryFile file)
        : state_(std::move(state)),
          file_(std::make_unique<sparenode::filesystem::TemporaryFile>(std::move(file)))
    {
    }

    ~StalledNativeBackedFile() override
    {
        file_.reset();
        state_->destroyed.set_value();
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(const std::span<const std::byte> bytes,
          const sparenode::filesystem::TemporaryFileIoOptions &options) override
    {
        state_->started.set_value();
        std::unique_lock lock(state_->mutex);
        state_->resume.wait(lock, [this] { return state_->released; });
        lock.unlock();
        return file_->write(bytes, options);
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete(const sparenode::filesystem::TemporaryFileIoOptions &options) override
    {
        return file_->complete(options);
    }

  private:
    std::shared_ptr<StalledWriteState> state_;
    std::unique_ptr<sparenode::filesystem::TemporaryFile> file_;
};

/// @brief Releases a simulated stalled system call after the request-worker assertion.
void resume_stalled_write(const std::shared_ptr<StalledWriteState> &state)
{
    {
        std::lock_guard lock(state->mutex);
        state->released = true;
    }
    state->resume.notify_all();
}

/// @brief Borrows string fixture storage as immutable bytes.
[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

/// @brief Reads the exact completed body owned by one ingestion artifact.
[[nodiscard]] std::string read_body(const sparenode::http::RequestBodyIngestor &ingestor)
{
    const auto artifact = ingestor.artifact();
    REQUIRE(artifact);
    REQUIRE(artifact->completed());
    std::ifstream input(artifact->path(), std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("Request body ingestor streams a fixed body larger than its decode buffer",
          "[http][body][temporary-file]")
{
    const std::string payload(20000, 'x');
    sparenode::http::RequestBodyIngestor ingestor({.content_length = payload.size()}, {});
    const auto progress = ingestor.feed(bytes(payload + "NEXT"));
    REQUIRE(progress.has_value());
    CHECK(progress->consumed == payload.size());
    CHECK(progress->produced == payload.size());
    REQUIRE(ingestor.complete());
    CHECK(ingestor.body_size() == payload.size());
    CHECK(read_body(ingestor) == payload);
}

TEST_CASE("Request body ingestor sends decoded chunk payload through the same file sink",
          "[http][body][chunked][temporary-file]")
{
    constexpr std::string_view wire = "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Ignored: yes\r\n\r\nNEXT";
    constexpr std::size_t framing_bytes = wire.size() - 4;
    sparenode::http::RequestBodyIngestor ingestor({.chunked = true}, {});
    const auto progress = ingestor.feed(bytes(wire));
    REQUIRE(progress.has_value());
    CHECK(progress->consumed == framing_bytes);
    CHECK(progress->produced == 9);
    REQUIRE(ingestor.complete());
    CHECK(ingestor.body_size() == 9);
    CHECK(read_body(ingestor) == "Wikipedia");
}

TEST_CASE("Request body ingestor completes an empty body without creating a file",
          "[http][body][temporary-file]")
{
    sparenode::http::RequestBodyIngestor ingestor({.content_length = 0}, {});
    CHECK(ingestor.complete());
    CHECK(ingestor.body_size() == 0);
    CHECK_FALSE(ingestor.artifact());
    REQUIRE(ingestor.finish().has_value());
}

TEST_CASE("Request body ingestor rejects fixed and decoded chunked bodies above the limit",
          "[http][body][limits][temporary-file]")
{
    SECTION("fixed length is rejected before payload bytes")
    {
        sparenode::http::HttpRequestParserLimits limits{};
        limits.max_body_bytes = 3;
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, limits);
        const auto result = ingestor.feed({});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::body_too_large);
        CHECK_FALSE(ingestor.artifact());
    }

    SECTION("chunked payload is bounded after decoding")
    {
        sparenode::http::HttpRequestParserLimits limits{};
        limits.max_body_bytes = 3;
        sparenode::http::RequestBodyIngestor ingestor({.chunked = true}, limits);
        const auto result = ingestor.feed(bytes("4\r\ndata\r\n0\r\n\r\n"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::body_too_large);
    }
}

TEST_CASE("Request body ingestor preserves premature EOF and cancellation failures",
          "[http][body][error][temporary-file]")
{
    SECTION("premature EOF remains a framing failure")
    {
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, {});
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        const auto failure = ingestor.finish();
        REQUIRE_FALSE(failure.has_value());
        CHECK(failure.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::framing_failure);
        const auto repeated = ingestor.finish();
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().code == failure.error().code);
        CHECK(repeated.error().framing_error.code == failure.error().framing_error.code);
        CHECK_FALSE(ingestor.artifact());
    }

    SECTION("cancellation remains distinguishable")
    {
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, {});
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        std::stop_source stop;
        stop.request_stop();
        const auto failure =
            ingestor.feed(bytes("cd"), {.stop_token = stop.get_token(), .deadline = {}});
        REQUIRE_FALSE(failure.has_value());
        CHECK(failure.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
        const auto repeated = ingestor.feed(bytes("cd"));
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().code == failure.error().code);
        CHECK_FALSE(ingestor.artifact());
    }
}

TEST_CASE("Request body ingestor propagates temporary file lifecycle failures",
          "[http][body][error][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    using sparenode::filesystem::TemporaryFileErrorCode;

    SECTION("creation")
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 1}, {},
            []() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError> {
                return sparenode::unexpected(
                    TemporaryFileError{TemporaryFileErrorCode::create_failed, 1234});
            });
        const auto result = ingestor.feed(bytes("x"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
        CHECK(result.error().file_error.code == TemporaryFileErrorCode::create_failed);
        CHECK(result.error().file_error.native_code == 1234);
    }

    SECTION("write")
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 1}, {},
            []() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
            {
                return std::static_pointer_cast<TemporaryFile>(
                    std::make_shared<FailingTemporaryFile>(TemporaryFileErrorCode::write_failed));
            });
        const auto result = ingestor.feed(bytes("x"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
        CHECK(result.error().file_error.code == TemporaryFileErrorCode::write_failed);
    }

    for (const auto code :
         {TemporaryFileErrorCode::flush_failed, TemporaryFileErrorCode::close_failed})
    {
        DYNAMIC_SECTION("completion failure " << static_cast<int>(code))
        {
            sparenode::http::RequestBodyIngestor ingestor(
                {.content_length = 1}, {},
                [code]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError> {
                    return std::static_pointer_cast<TemporaryFile>(
                        std::make_shared<FailingTemporaryFile>(code));
                });
            const auto result = ingestor.feed(bytes("x"));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code ==
                  sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
            CHECK(result.error().file_error.code == code);
        }
    }
}

TEST_CASE("Cancelled ingestion destroys and removes its partial native artifact",
          "[http][body][cancel][temporary-file]")
{
    std::filesystem::path path;
    std::weak_ptr<sparenode::filesystem::TemporaryFile> observer;
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 4}, {},
            [&]() -> sparenode::Result<std::shared_ptr<sparenode::filesystem::TemporaryFile>,
                                       sparenode::filesystem::TemporaryFileError>
            {
                auto created = sparenode::filesystem::TemporaryFile::create();
                if (!created)
                {
                    return sparenode::unexpected(created.error());
                }
                auto artifact = std::make_shared<sparenode::filesystem::TemporaryFile>(
                    std::move(created).value());
                path = artifact->path();
                observer = artifact;
                return artifact;
            });
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        REQUIRE(std::filesystem::exists(path));
        std::stop_source stop;
        stop.request_stop();
        const auto result =
            ingestor.feed(bytes("cd"), {.stop_token = stop.get_token(), .deadline = {}});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
        CHECK(observer.expired());
        CHECK_FALSE(std::filesystem::exists(path));
    }
    CHECK(observer.expired());
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Cancellation interrupts an injected blocked body write and releases its artifact",
          "[http][body][cancel][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    auto state = std::make_shared<BlockedWriteState>();
    auto started = state->started.get_future();
    sparenode::http::RequestBodyIngestor ingestor(
        {.content_length = 1}, {},
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<BlockingTemporaryFile>(state));
        });
    std::stop_source stop;
    auto result = std::async(std::launch::async,
                             [&]
                             {
                                 return ingestor.feed(
                                     bytes("x"), {.stop_token = stop.get_token(),
                                                  .deadline = std::chrono::steady_clock::now() +
                                                              std::chrono::seconds{5}});
                             });
    const auto started_in_time =
        started.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    stop.request_stop();
    REQUIRE(started_in_time);
    REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    const auto failure = result.get();
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
    CHECK(state->destroyed.load(std::memory_order_acquire));
    CHECK_FALSE(ingestor.artifact());
}

TEST_CASE("Deadline interrupts an injected blocked body write and releases its artifact",
          "[http][body][timeout][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    auto state = std::make_shared<BlockedWriteState>();
    auto started = state->started.get_future();
    sparenode::http::RequestBodyIngestor ingestor(
        {.content_length = 1}, {},
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<BlockingTemporaryFile>(state));
        });
    std::stop_source stop;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    auto result = std::async(std::launch::async,
                             [&] {
                                 return ingestor.feed(bytes("x"), {.stop_token = stop.get_token(),
                                                                   .deadline = deadline});
                             });
    const auto started_in_time =
        started.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    const auto finished_in_time =
        result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!started_in_time || !finished_in_time)
    {
        stop.request_stop();
    }
    REQUIRE(started_in_time);
    REQUIRE(finished_in_time);
    const auto failure = result.get();
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code ==
          sparenode::http::RequestBodyIngestionErrorCode::deadline_exceeded);
    CHECK(state->destroyed.load(std::memory_order_acquire));
    CHECK_FALSE(ingestor.artifact());
}

TEST_CASE("Cancellation releases the request worker while a native file write remains stalled",
          "[http][body][cancel][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    auto state = std::make_shared<StalledWriteState>();
    auto started = state->started.get_future();
    auto destroyed = state->destroyed.get_future();
    sparenode::http::RequestBodyIngestor ingestor(
        {.content_length = 1}, {},
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            auto created = TemporaryFile::create();
            if (!created)
            {
                return sparenode::unexpected(created.error());
            }
            state->path = created->path();
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<StalledNativeBackedFile>(state, std::move(created).value()));
        });
    std::stop_source stop;
    auto result =
        std::async(std::launch::async,
                   [&] { return ingestor.feed(bytes("x"), {.stop_token = stop.get_token()}); });
    const auto started_in_time =
        started.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    stop.request_stop();
    const auto returned_before_native_write =
        result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    const auto retained_until_write_returns =
        started_in_time && std::filesystem::exists(state->path);
    resume_stalled_write(state);
    REQUIRE(started_in_time);
    REQUIRE(returned_before_native_write);
    const auto failure = result.get();
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
    CHECK(retained_until_write_returns);
    CHECK(destroyed.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK_FALSE(std::filesystem::exists(state->path));
    CHECK_FALSE(ingestor.artifact());
}

TEST_CASE("Deadline releases the request worker while a native file write remains stalled",
          "[http][body][timeout][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    auto state = std::make_shared<StalledWriteState>();
    auto started = state->started.get_future();
    auto destroyed = state->destroyed.get_future();
    sparenode::http::RequestBodyIngestor ingestor(
        {.content_length = 1}, {},
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError> {
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<StalledTemporaryFile>(state));
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    auto result = std::async(std::launch::async,
                             [&] { return ingestor.feed(bytes("x"), {.deadline = deadline}); });
    const auto started_in_time =
        started.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    const auto returned_before_native_write =
        result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    resume_stalled_write(state);
    REQUIRE(started_in_time);
    REQUIRE(returned_before_native_write);
    const auto failure = result.get();
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code ==
          sparenode::http::RequestBodyIngestionErrorCode::deadline_exceeded);
    CHECK(destroyed.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK_FALSE(ingestor.artifact());
}

TEST_CASE("Deadline releases the request worker while native file completion remains stalled",
          "[http][body][timeout][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    auto state = std::make_shared<StalledWriteState>();
    auto started = state->started.get_future();
    auto destroyed = state->destroyed.get_future();
    sparenode::http::RequestBodyIngestor ingestor(
        {.content_length = 1}, {},
        [state]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
        {
            return std::static_pointer_cast<TemporaryFile>(
                std::make_shared<StalledCompletionFile>(state));
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    auto result = std::async(std::launch::async,
                             [&] { return ingestor.feed(bytes("x"), {.deadline = deadline}); });
    const auto started_in_time =
        started.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    const auto returned_before_native_completion =
        result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    resume_stalled_write(state);
    REQUIRE(started_in_time);
    REQUIRE(returned_before_native_completion);
    const auto failure = result.get();
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code ==
          sparenode::http::RequestBodyIngestionErrorCode::deadline_exceeded);
    CHECK(destroyed.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK_FALSE(ingestor.artifact());
}
