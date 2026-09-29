#include "sparenode/http/session/detail/buffered_request.hpp"

#include <array>
#include <string_view>

#include "sparenode/http/detail/ascii.hpp"
#include "sparenode/http/request/detail/body_decode_buffer.hpp"

namespace sparenode::http::detail
{
namespace
{
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
Result<void, HttpRequestParseError> BufferedRequest::feed(const std::span<const std::byte> input)
{
    if (head_.has_value())
    {
        return feed_body(input);
    }
    metadata_.insert(metadata_.end(), input.begin(), input.end());
    auto parsed = parse_http_request_head(metadata_, limits_);
    if (!parsed)
    {
        return unexpected(parsed.error());
    }
    if (!parsed->has_value())
    {
        return {};
    }
    auto head = std::move(parsed.value()).value_or(HttpRequestHead{});
    const auto head_bytes = head.consumed_bytes;
    decoder_.emplace(head, limits_);
    head_ = std::move(head);
    const auto result = feed_body(std::span(metadata_).subspan(head_bytes));
    metadata_.resize(head_bytes);
    return result;
}

/// @brief Copies only emitted payload; chunk metadata and trailers never enter body storage.
Result<void, HttpRequestParseError> BufferedRequest::feed_body(std::span<const std::byte> input)
{
    if (!decoder_.has_value())
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::incomplete_body, 0});
    }
    std::array<std::byte, body_decode_buffer_bytes> output{};
    while (!input.empty() && !decoder_->complete())
    {
        const auto progress = decoder_->feed(input, output);
        if (!progress)
        {
            return unexpected(progress.error());
        }
        body_.insert(body_.end(), output.begin(),
                     output.begin() + static_cast<std::ptrdiff_t>(progress->produced));
        input = input.subspan(progress->consumed);
    }
    return {};
}
} // namespace sparenode::http::detail
