#include "sparenode/http/transport/detail/buffered_request.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "sparenode/foundation/memory/secure_erase.hpp"
#include "sparenode/http/detail/ascii.hpp"

namespace sparenode::http::detail
{
namespace
{

/// @brief Erases a borrowed sensitive byte range before its vector releases logical ownership.
class SensitiveByteEraser final
{
  public:
    explicit SensitiveByteEraser(const std::span<std::byte> bytes) noexcept : bytes_(bytes)
    {
    }

    ~SensitiveByteEraser()
    {
        memory::secure_erase(bytes_);
    }

    SensitiveByteEraser(const SensitiveByteEraser &) = delete;
    SensitiveByteEraser &operator=(const SensitiveByteEraser &) = delete;

  private:
    std::span<std::byte> bytes_; ///< Live coalesced body range owned by BufferedRequest.
};

/// @brief Accepts a bounded list of 100-continue tokens, ignoring empty list members.
[[nodiscard]] bool supported_expectations(std::string_view value)
{
    while (!value.empty())
    {
        const auto comma = value.find(',');
        auto item = value.substr(0, comma);
        const auto begin = item.find_first_not_of(" \t");
        if (begin != std::string_view::npos)
        {
            item = item.substr(begin, item.find_last_not_of(" \t") - begin + 1);
            if (!ascii_case_insensitive_equal(item, "100-continue"))
            {
                return false;
            }
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return true;
}
} // namespace

BufferedRequest::~BufferedRequest()
{
    erase_metadata();
}

void BufferedRequest::append_metadata(const std::span<const std::byte> input)
{
    if (input.size() > metadata_.max_size() - metadata_.size())
    {
        throw std::length_error("HTTP request metadata exceeds vector capacity");
    }
    const auto required = metadata_.size() + input.size();
    if (required <= metadata_.capacity())
    {
        metadata_.insert(metadata_.end(), input.begin(), input.end());
        return;
    }

    auto replacement_capacity = (std::max)(metadata_.capacity(), input.size());
    while (replacement_capacity < required)
    {
        if (replacement_capacity > metadata_.max_size() - replacement_capacity)
        {
            replacement_capacity = required;
            break;
        }
        replacement_capacity += replacement_capacity;
    }

    std::vector<std::byte> replacement;
    replacement.reserve(replacement_capacity);
    replacement.insert(replacement.end(), metadata_.begin(), metadata_.end());
    replacement.insert(replacement.end(), input.begin(), input.end());
    memory::secure_erase(metadata_);
    metadata_.swap(replacement);
}

void BufferedRequest::erase_metadata() noexcept
{
    memory::secure_erase(metadata_);
    metadata_.clear();
}

/// @brief Returns the pending Expect decision once and clears it from the request state.
RequestExpectation BufferedRequest::take_expectation()
{
    if (!head_.has_value() || expectation_checked_)
    {
        return RequestExpectation::none;
    }
    expectation_checked_ = true;
    bool expected = false;
    for (const auto &field : head_->fields)
    {
        if (ascii_case_insensitive_equal(field.name, "Expect"))
        {
            if (!supported_expectations(field.value))
            {
                return RequestExpectation::unsupported;
            }
            expected = expected || field.value.find_first_not_of(" ,\t") != std::string_view::npos;
        }
    }
    return expected && !complete() ? RequestExpectation::continue_request
                                   : RequestExpectation::none;
}

/// @brief Parses metadata once complete, then feeds all subsequent bytes to the body decoder.
Result<void, BufferedRequestError>
BufferedRequest::feed(const std::span<std::byte> input,
                      const storage::TemporaryFileIoOptions &options)
{
    return feed_with_body_options(input, [&options] { return options; });
}

/// @brief Defers the file deadline provider until validated metadata reaches body ingestion.
Result<void, BufferedRequestError> BufferedRequest::feed_with_body_options(
    const std::span<std::byte> input,
    const std::function<storage::TemporaryFileIoOptions()> &options_provider)
{
    const SensitiveByteEraser erase_input(input);
    if (head_.has_value())
    {
        return feed_body(input, options_provider());
    }
    append_metadata(input);
    auto parsed = parse_http_request_head(metadata_, limits_);
    if (!parsed)
    {
        const auto error = parsed.error();
        erase_metadata();
        return unexpected(BufferedRequestError(error));
    }
    if (!parsed->has_value())
    {
        return {};
    }
    auto head = std::move(parsed.value()).value_or(HttpRequestHead{});
    const auto head_bytes = head.consumed_bytes;
    const auto storage_policy =
        storage_policy_selector_ ? storage_policy_selector_(head) : RequestBodyStoragePolicy{};
    auto body_limits = limits_;
    if (storage_policy.max_body_bytes != 0)
    {
        body_limits.max_body_bytes =
            (std::min)(body_limits.max_body_bytes, storage_policy.max_body_bytes);
    }
    ingestor_.emplace(head, body_limits, file_factory_, storage_policy.mode);
    head_ = std::move(head);
    const auto result = [&]
    {
        const auto body = std::span(metadata_).subspan(head_bytes);
        const SensitiveByteEraser erase_body(body);
        return feed_body(body, options_provider());
    }();
    metadata_.resize(head_bytes);
    return result;
}

/// @brief Persists only emitted payload; chunk metadata and trailers never enter body storage.
Result<void, BufferedRequestError>
BufferedRequest::feed_body(const std::span<const std::byte> input,
                           const storage::TemporaryFileIoOptions &options)
{
    if (!ingestor_.has_value())
    {
        return unexpected(BufferedRequestError(
            HttpRequestParseError{HttpRequestParseErrorCode::incomplete_body, 0}));
    }
    const auto progress = ingestor_->feed(input, options);
    if (!progress)
    {
        return unexpected(BufferedRequestError(progress.error()));
    }
    return {};
}

/// @brief Validates EOF and finalizes the artifact under the active I/O policy.
Result<void, BufferedRequestError>
BufferedRequest::finish(const storage::TemporaryFileIoOptions &options)
{
    if (!ingestor_)
    {
        return unexpected(BufferedRequestError(
            HttpRequestParseError{HttpRequestParseErrorCode::incomplete_body, metadata_.size()}));
    }
    const auto result = ingestor_->finish(options);
    if (!result)
    {
        return unexpected(BufferedRequestError(result.error()));
    }
    return {};
}
} // namespace sparenode::http::detail
