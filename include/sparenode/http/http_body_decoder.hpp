#pragma once

#include "sparenode/http/http_request_parser.hpp"
#include <string>

namespace sparenode::http
{
/// @brief Reports one incremental transfer into caller-owned payload storage.
struct HttpBodyDecodeProgress
{
    std::size_t consumed{}; ///< Wire bytes consumed; pipelined bytes remain untouched.
    std::size_t produced{}; ///< Decoded bytes written to the output span.
    bool complete{};        ///< Entire body including trailer terminator has arrived.
};

/// @brief Decodes fixed-length or chunked payloads without owning body storage or doing I/O.
/// Callers retain unconsumed input and commit produced output only after success.
/// Discard the complete request on any error; no further feed is accepted after failure.
class HttpBodyDecoder final
{
  public:
    /// @brief Initializes ingestion from validated framing metadata.
    /// @param[in] head Validated request framing and absolute body offset.
    /// @param[in] limits Decoded payload and chunk metadata limits.
    HttpBodyDecoder(const HttpRequestHead &head, const HttpRequestParserLimits &limits);

    /// @brief Consumes new wire bytes once and emits payload into bounded caller storage.
    /// @param[in] input New bytes, starting at the previous unconsumed boundary.
    /// @param[out] output Nonoverlapping payload destination; may be smaller than input.
    /// @return Progress or terminal protocol error with absolute request offset.
    [[nodiscard]] Result<HttpBodyDecodeProgress, HttpRequestParseError>
    feed(std::span<const std::byte> input, std::span<std::byte> output);

    /// @brief Validates EOF without treating an unfinished stream as successful.
    /// @return Success only after the complete framing boundary, or a terminal error.
    [[nodiscard]] Result<void, HttpRequestParseError> finish() noexcept;

    /// @brief Reports whether the complete framing boundary has been consumed.
    /// @return True after fixed-length bytes or the final chunk trailer terminator.
    [[nodiscard]] bool complete() const noexcept;

  private:
    /// @brief Identifies the next wire element required for complete framing.
    enum class Phase : std::uint8_t
    {
        size_line, ///< Reading a size line.
        data,      ///< Copying payload.
        data_cr,   ///< Awaiting CR after payload.
        data_lf,   ///< Awaiting LF after payload.
        trailers,  ///< Reading and discarding trailer fields.
        done       ///< Entire body is complete.
    };

    /// @brief Consumes one bounded framing byte and advances the chunk state.
    /// @param[in] byte Next wire byte at the current absolute offset.
    /// @return Success or a metadata syntax/limit failure.
    [[nodiscard]] Result<void, HttpRequestParseError> metadata(char byte);
    /// @brief Applies a complete size or trailer line, excluding its CRLF.
    /// @return Success or a size, extension, or trailer validation failure.
    [[nodiscard]] Result<void, HttpRequestParseError> finish_line();
    /// @brief Copies as many payload bytes as both spans permit.
    /// @param[in] input Wire fragment containing the next payload bytes.
    /// @param[out] output Caller-owned decoded destination.
    /// @param[in,out] progress Consumed and produced offsets within the supplied spans.
    void copy_data(std::span<const std::byte> input, std::span<std::byte> output,
                   HttpBodyDecodeProgress &progress);
    /// @brief Forms a failure at the current absolute wire offset.
    /// @param[in] code Structured protocol failure category.
    /// @return Error preserving the current absolute request offset.
    [[nodiscard]] HttpRequestParseError error(HttpRequestParseErrorCode code) const noexcept;

    HttpRequestParserLimits limits_; ///< Independent caller-supplied limits.
    Phase phase_;                    ///< Current framing boundary.
    bool chunked_;                   ///< Fixed or chunked body policy.
    std::size_t remaining_;          ///< Unconsumed payload bytes in the current chunk or body.
    std::size_t decoded_{};          ///< Cumulative emitted payload bytes.
    std::size_t offset_;             ///< Absolute wire offset, including request head.
    std::size_t metadata_bytes_{};   ///< Cumulative framing overhead.
    std::size_t trailer_bytes_{};    ///< Cumulative trailer bytes including CRLF.
    std::size_t trailer_count_{};    ///< Number of discarded trailer fields.
    std::string line_;               ///< At most one bounded metadata line.
    bool line_cr_{};                 ///< A line's CR has arrived and requires LF.
    std::optional<HttpRequestParseError> failure_; ///< Sticky terminal error.
};
} // namespace sparenode::http
