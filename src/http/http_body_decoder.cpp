#include "sparenode/http/http_body_decoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace sparenode::http
{
namespace
{
/// @brief Tests the ASCII HTTP token grammar without locale conversion.
[[nodiscard]] bool token(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           std::string_view("!#$%&'*+-.^_`|~").find(byte) != std::string_view::npos;
}

/// @brief Removes grammar-permitted bad whitespace around extension separators.
void skip_whitespace(std::string_view &text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
}

/// @brief Consumes a nonempty extension token.
[[nodiscard]] bool take_token(std::string_view &text) noexcept
{
    const auto end = std::ranges::find_if_not(text, token);
    const auto count = static_cast<std::size_t>(end - text.begin());
    text.remove_prefix(count);
    return count != 0;
}

/// @brief Consumes a quoted extension value, validating quoted pairs and text.
[[nodiscard]] bool take_quoted(std::string_view &text) noexcept
{
    text.remove_prefix(1);
    while (!text.empty())
    {
        auto byte = static_cast<unsigned char>(text.front());
        text.remove_prefix(1);
        if (byte == '"')
        {
            return true;
        }
        if (byte == '\\')
        {
            if (text.empty())
            {
                return false;
            }
            byte = static_cast<unsigned char>(text.front());
            text.remove_prefix(1);
        }
        if ((byte < 0x20U && byte != '\t') || byte == 0x7FU)
        {
            return false;
        }
    }
    return false;
}

/// @brief Validates and ignores bounded chunk extensions, including quoted values.
[[nodiscard]] bool valid_extensions(std::string_view text) noexcept
{
    while (!text.empty())
    {
        skip_whitespace(text);
        if (text.empty() || text.front() != ';')
        {
            return false;
        }
        text.remove_prefix(1);
        skip_whitespace(text);
        if (!take_token(text))
        {
            return false;
        }
        // Whitespace belongs to the following separator, not to the token itself.
        auto after_name = text;
        skip_whitespace(after_name);
        if (!after_name.empty() && after_name.front() == '=')
        {
            text = after_name.substr(1);
            skip_whitespace(text);
            if (text.empty() || !(text.front() == '"' ? take_quoted(text) : take_token(text)))
            {
                return false;
            }
        }
    }
    return true;
}

/// @brief Validates discarded trailers and rejects fields affecting framing or authorization.
[[nodiscard]] bool valid_trailer(const std::string_view line)
{
    const auto colon = line.find(':');
    if (colon == 0 || colon == std::string_view::npos ||
        !std::ranges::all_of(line.substr(0, colon), token))
    {
        return false;
    }
    std::string name(line.substr(0, colon));
    std::ranges::transform(
        name, name.begin(), [](const char byte)
        { return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte; });
    constexpr std::array<std::string_view, 14> forbidden{"content-length", "transfer-encoding",
                                                         "host",           "connection",
                                                         "trailer",        "te",
                                                         "authorization",  "proxy-authorization",
                                                         "cookie",         "expect",
                                                         "upgrade",        "content-encoding",
                                                         "content-type",   "content-range"};
    if (std::ranges::find(forbidden, name) != std::end(forbidden))
    {
        return false;
    }
    return std::ranges::none_of(line.substr(colon + 1), [](const unsigned char byte)
                                { return (byte < 0x20U && byte != '\t') || byte == 0x7FU; });
}
} // namespace

/// @brief Selects a bounded framing state from the validated request head.
HttpBodyDecoder::HttpBodyDecoder(const HttpRequestHead &head, const HttpRequestParserLimits &limits)
    : limits_(limits),
      phase_(head.chunked ? Phase::size_line
                          : (head.content_length == 0 ? Phase::done : Phase::data)),
      chunked_(head.chunked), remaining_(head.content_length), offset_(head.consumed_bytes)
{
    if (!chunked_ && remaining_ > limits_.max_body_bytes)
    {
        failure_ = error(HttpRequestParseErrorCode::body_too_large);
    }
}

/// @brief Forms an absolute diagnostic offset without exposing request contents.
HttpRequestParseError HttpBodyDecoder::error(const HttpRequestParseErrorCode code) const noexcept
{
    return {code, offset_};
}

/// @brief Reports successful completion only when no earlier failure exists.
bool HttpBodyDecoder::complete() const noexcept
{
    return phase_ == Phase::done && !failure_.has_value();
}

/// @brief Rejects EOF in every incomplete framing state.
Result<void, HttpRequestParseError> HttpBodyDecoder::finish() noexcept
{
    if (failure_.has_value())
    {
        return unexpected(failure_.value());
    }
    if (!complete())
    {
        failure_ = error(HttpRequestParseErrorCode::incomplete_body);
        return unexpected(failure_.value());
    }
    return {};
}

/// @brief Validates a completed size or trailer line before selecting the next phase.
Result<void, HttpRequestParseError> HttpBodyDecoder::finish_line()
{
    if (phase_ == Phase::trailers)
    {
        if (line_.empty())
        {
            phase_ = Phase::done;
            return {};
        }
        if (trailer_count_ >= limits_.max_trailer_count)
        {
            return unexpected(error(HttpRequestParseErrorCode::trailers_too_large));
        }
        ++trailer_count_;
        if (!valid_trailer(line_))
        {
            return unexpected(error(HttpRequestParseErrorCode::invalid_trailer));
        }
        return {};
    }
    std::size_t size = 0;
    const auto converted = std::from_chars(line_.data(), line_.data() + line_.size(), size, 16);
    if (converted.ec != std::errc{} ||
        !valid_extensions(std::string_view(
            converted.ptr, static_cast<std::size_t>(line_.data() + line_.size() - converted.ptr))))
    {
        return unexpected(error(HttpRequestParseErrorCode::malformed_chunk));
    }
    if (size > limits_.max_body_bytes - decoded_)
    {
        return unexpected(error(HttpRequestParseErrorCode::body_too_large));
    }
    remaining_ = size;
    phase_ = size == 0 ? Phase::trailers : Phase::data;
    return {};
}

/// @brief Accounts for each framing byte before retaining or interpreting it.
Result<void, HttpRequestParseError> HttpBodyDecoder::metadata(const char byte)
{
    if (metadata_bytes_ >= limits_.max_chunk_metadata_bytes)
    {
        return unexpected(error(HttpRequestParseErrorCode::chunk_metadata_too_large));
    }
    ++metadata_bytes_;
    if (phase_ == Phase::data_cr || phase_ == Phase::data_lf)
    {
        const bool needs_cr = phase_ == Phase::data_cr;
        if (byte != (needs_cr ? '\r' : '\n'))
        {
            return unexpected(error(HttpRequestParseErrorCode::malformed_chunk));
        }
        phase_ = needs_cr ? Phase::data_lf : Phase::size_line;
        return {};
    }
    if (phase_ == Phase::trailers)
    {
        if (trailer_bytes_ >= limits_.max_trailer_bytes)
        {
            return unexpected(error(HttpRequestParseErrorCode::trailers_too_large));
        }
        ++trailer_bytes_;
    }
    if (line_cr_)
    {
        if (byte != '\n')
        {
            return unexpected(error(HttpRequestParseErrorCode::malformed_chunk));
        }
        auto result = finish_line();
        line_.clear();
        line_cr_ = false;
        return result;
    }
    if (byte == '\r')
    {
        line_cr_ = true;
        return {};
    }
    if (byte == '\n')
    {
        return unexpected(error(HttpRequestParseErrorCode::malformed_chunk));
    }
    const auto limit =
        phase_ == Phase::trailers ? limits_.max_trailer_bytes : limits_.max_chunk_line_bytes;
    if (line_.size() >= limit)
    {
        return unexpected(error(HttpRequestParseErrorCode::chunk_metadata_too_large));
    }
    line_.push_back(byte);
    return {};
}

/// @brief Copies payload only, leaving chunk boundaries for the metadata state machine.
void HttpBodyDecoder::copy_data(const std::span<const std::byte> input,
                                const std::span<std::byte> output, HttpBodyDecodeProgress &progress)
{
    const auto count =
        (std::min)({remaining_, input.size() - progress.consumed, output.size() - progress.produced,
                    (std::numeric_limits<std::size_t>::max)() - offset_});
    std::ranges::copy(input.subspan(progress.consumed, count),
                      output.subspan(progress.produced).begin());
    remaining_ -= count;
    decoded_ += count;
    offset_ += count;
    progress.consumed += count;
    progress.produced += count;
    if (remaining_ == 0)
    {
        phase_ = chunked_ ? Phase::data_cr : Phase::done;
    }
}

/// @brief Processes each input byte at most once and stops at output or request boundaries.
Result<HttpBodyDecodeProgress, HttpRequestParseError>
HttpBodyDecoder::feed(const std::span<const std::byte> input, const std::span<std::byte> output)
{
    if (failure_.has_value())
    {
        return unexpected(failure_.value());
    }
    HttpBodyDecodeProgress progress;
    while (progress.consumed < input.size() && phase_ != Phase::done)
    {
        if (offset_ == (std::numeric_limits<std::size_t>::max)())
        {
            failure_ = error(HttpRequestParseErrorCode::chunk_metadata_too_large);
            return unexpected(failure_.value());
        }
        if (phase_ == Phase::data)
        {
            if (progress.produced == output.size())
            {
                break;
            }
            copy_data(input, output, progress);
            continue;
        }
        const auto result = metadata(static_cast<char>(input[progress.consumed]));
        if (!result)
        {
            failure_ = result.error();
            return unexpected(failure_.value());
        }
        ++progress.consumed;
        ++offset_;
    }
    progress.complete = complete();
    return progress;
}
} // namespace sparenode::http
