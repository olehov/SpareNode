#include "sparenode/http/detail/buffered_request.hpp"

#include <array>

#include "sparenode/http/detail/body_decode_buffer.hpp"

namespace sparenode::http::detail
{
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
