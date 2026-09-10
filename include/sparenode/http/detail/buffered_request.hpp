#pragma once

#include <cstdint>
#include <stdexcept>

#include "sparenode/http/detail/http_request_view_access.hpp"
#include "sparenode/http/http_body_decoder.hpp"

namespace sparenode::http::detail
{
/// @brief One-time action selected after validated request headers arrive.
enum class RequestExpectation : std::uint8_t
{
    none,             ///< No interim or final expectation action is required.
    continue_request, ///< A supported expectation awaits additional body bytes.
    unsupported       ///< At least one expectation cannot be met.
};

/// @brief Adapts streaming body ingestion to the MVP's bounded in-memory route contract.
/// Metadata storage freezes after parsing; body bytes use one decoder for both framings.
class BufferedRequest final
{
  public:
    /// @brief Retains the limits for one session-owned request.
    /// @param[in] limits Request metadata, decoded payload, and framing budgets.
    explicit BufferedRequest(const HttpRequestParserLimits &limits) : limits_(limits)
    {
    }

    /// @brief Adds newly received wire bytes, retaining only metadata and decoded payload.
    /// @param[in] input Fresh receive bytes; trailing pipelined bytes are ignored after completion.
    /// @return Success or a structured terminal parse/body error.
    [[nodiscard]] Result<void, HttpRequestParseError> feed(std::span<const std::byte> input);

    /// @brief Reports the transition to body ingestion for inactivity accounting.
    /// @return True once a valid complete request head is retained.
    [[nodiscard]] bool reading_body() const noexcept
    {
        return head_.has_value();
    }
    /// @brief Reports that body framing, including trailers, has completed.
    /// @return True when the body decoder reaches its final boundary without failure.
    [[nodiscard]] bool complete() const noexcept
    {
        return decoder_.has_value() && decoder_->complete();
    }
    /// @brief Reports whether the peer has sent no request bytes at all.
    /// @return True before receiving any request metadata.
    [[nodiscard]] bool empty() const noexcept
    {
        return metadata_.empty();
    }
    /// @brief Builds a request borrowing this adapter's stable storage after completion.
    /// @return Complete request; callers must first check complete().
    [[nodiscard]] HttpRequestView request() const
    {
        if (!head_.has_value() || !complete())
        {
            throw std::logic_error("HTTP request is incomplete");
        }
        return HttpRequestViewAccess::create(head_.value(), body_);
    }

    /// @brief Consumes the header expectation decision once, before waiting for more body bytes.
    /// @return Continue for an incomplete expected body, unsupported for unknown expectations.
    [[nodiscard]] RequestExpectation take_expectation();

  private:
    /// @brief Drains decoder output into the bounded MVP payload buffer.
    /// @param[in] input New body wire bytes, excluding previously consumed data.
    /// @return Success or terminal body syntax/limit failure.
    [[nodiscard]] Result<void, HttpRequestParseError> feed_body(std::span<const std::byte> input);

    bool expectation_checked_{};      ///< Prevents repeated interim responses.
    HttpRequestParserLimits limits_;  ///< Session parser and decoder limits.
    std::vector<std::byte> metadata_; ///< Frozen metadata backing all header views.
    std::vector<std::byte> body_;     ///< Bounded decoded payload; replaceable by SN-087 ingestion.
    std::optional<HttpRequestHead> head_;    ///< Validated borrowed request metadata.
    std::optional<HttpBodyDecoder> decoder_; ///< Persistent body framing state.
};
} // namespace sparenode::http::detail
