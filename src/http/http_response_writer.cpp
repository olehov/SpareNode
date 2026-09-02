#include "sparenode/http/http_response_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>

namespace sparenode::http
{

/// @brief Restricts mutable body-reader access to the response writer.
class HttpResponseWriterAccess final
{
  public:
    /// @brief Advances the private stateful reader owned by a streaming response.
    /// @param[in,out] response Response whose reader will advance.
    /// @param[out] destination Caller-bounded storage receiving the next chunk.
    /// @param[in] stop_token Token observed by the body source.
    /// @return Produced byte count or the body-source failure.
    [[nodiscard]] static Result<std::size_t, HttpBodyReadError>
    read_body(HttpResponse &response, const std::span<std::byte> destination,
              const std::stop_token &stop_token)
    {
        return response.body_reader_(destination, stop_token);
    }
};

namespace
{

constexpr std::size_t stream_buffer_size = std::size_t{16} * 1024;
constexpr std::uint16_t minimum_response_status_code = 100;

/// @brief Identifies statuses whose response head must omit Content-Length.
[[nodiscard]] constexpr bool status_omits_content_length(const HttpStatusCode status_code) noexcept
{
    const std::uint16_t value = http_status_code_value(status_code);
    return (value >= minimum_response_status_code &&
            value < http_status_code_value(HttpStatusCode::ok)) ||
           status_code == HttpStatusCode::no_content || status_code == HttpStatusCode::not_modified;
}

/// @brief Counts decimal digits in one unsigned framing value.
[[nodiscard]] constexpr std::size_t decimal_length(std::uint64_t value) noexcept
{
    std::size_t digits = 1;
    while (value >= 10)
    {
        value /= 10;
        ++digits;
    }
    return digits;
}

/// @brief Computes the exact allocation required for a validated response head.
[[nodiscard]] std::size_t serialized_head_size(const HttpResponse &response) noexcept
{
    constexpr std::size_t status_line_fixed_bytes = 15;
    std::size_t size = status_line_fixed_bytes + response.reason_phrase().size() + 2;
    size += std::accumulate(response.headers().begin(), response.headers().end(), std::size_t{0},
                            [](const std::size_t current, const HttpResponseHeader &header)
                            { return current + header.name.size() + header.value.size() + 4; });
    if (!status_omits_content_length(response.status_code()))
    {
        constexpr std::size_t content_length_fixed_bytes = 18;
        size += content_length_fixed_bytes + decimal_length(response.content_length());
    }
    return size;
}

/// @brief Repeats bounded connection writes until one complete span is delivered.
[[nodiscard]] Result<void, HttpResponseWriteError> send_all(network::TcpConnection &connection,
                                                            std::span<const std::byte> bytes,
                                                            const std::stop_token &stop_token)
{
    while (!bytes.empty())
    {
        auto sent = connection.send(bytes, stop_token);
        if (!sent)
        {
            return unexpected(HttpResponseWriteError{HttpResponseWriteErrorCode::network_failure,
                                                     sent.error(), std::nullopt});
        }
        if (sent.value() == 0)
        {
            return unexpected(HttpResponseWriteError{HttpResponseWriteErrorCode::connection_closed,
                                                     std::nullopt, std::nullopt});
        }
        bytes = bytes.subspan(sent.value());
    }
    return {};
}

/// @brief Pulls fixed-size chunks until the declared body length is transmitted.
[[nodiscard]] Result<void, HttpResponseWriteError>
write_streaming_body(network::TcpConnection &connection, HttpResponse &response,
                     const std::stop_token &stop_token)
{
    std::array<std::byte, stream_buffer_size> buffer{};
    std::uint64_t remaining = response.content_length();
    while (remaining > 0)
    {
        const std::size_t requested = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
        Result<std::size_t, HttpBodyReadError> produced =
            unexpected(HttpBodyReadError{HttpBodyReadErrorDomain::application, 0});
        try
        {
            produced = HttpResponseWriterAccess::read_body(
                response, std::span(buffer).first(requested), stop_token);
        }
        catch (...)
        {
            return unexpected(HttpResponseWriteError{
                HttpResponseWriteErrorCode::body_source_exception, std::nullopt, std::nullopt});
        }
        if (!produced)
        {
            return unexpected(HttpResponseWriteError{
                HttpResponseWriteErrorCode::body_source_failure, std::nullopt, produced.error()});
        }
        if (produced.value() > requested)
        {
            return unexpected(
                HttpResponseWriteError{HttpResponseWriteErrorCode::body_source_contract_violation,
                                       std::nullopt, std::nullopt});
        }
        if (produced.value() == 0)
        {
            return unexpected(HttpResponseWriteError{HttpResponseWriteErrorCode::body_ended_early,
                                                     std::nullopt, std::nullopt});
        }
        if (auto sent = send_all(connection, std::span(buffer).first(produced.value()), stop_token);
            !sent)
        {
            return sent;
        }
        remaining -= produced.value();
    }
    return {};
}

} // namespace

/// @brief Produces a complete validated response head with deterministic framing.
std::string serialize_http_response_head(const HttpResponse &response)
{
    std::string head;
    head.reserve(serialized_head_size(response));
    head += "HTTP/1.1 ";
    head += std::to_string(http_status_code_value(response.status_code()));
    head += ' ';
    head += response.reason_phrase();
    head += "\r\n";
    for (const auto &header : response.headers())
    {
        head += header.name;
        head += ": ";
        head += header.value;
        head += "\r\n";
    }
    if (!status_omits_content_length(response.status_code()))
    {
        head += "Content-Length: ";
        head += std::to_string(response.content_length());
        head += "\r\n";
    }
    head += "\r\n";
    return head;
}

/// @brief Transmits a response head followed by its memory or streaming body.
Result<void, HttpResponseWriteError> write_http_response(network::TcpConnection &connection,
                                                         HttpResponse &response,
                                                         const std::stop_token &stop_token)
{
    try
    {
        const std::string head = serialize_http_response_head(response);
        const auto head_bytes = std::as_bytes(std::span(head.data(), head.size()));
        if (auto sent = send_all(connection, head_bytes, stop_token); !sent)
        {
            return sent;
        }
        if (response.is_streaming())
        {
            return write_streaming_body(connection, response, stop_token);
        }
        return send_all(connection, response.memory_body(), stop_token);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpResponseWriteError{
            HttpResponseWriteErrorCode::resource_allocation_failed, std::nullopt, std::nullopt});
    }
    catch (const std::length_error &)
    {
        return unexpected(HttpResponseWriteError{
            HttpResponseWriteErrorCode::resource_allocation_failed, std::nullopt, std::nullopt});
    }
}

/// @brief Converts a response-write category to stable diagnostic text.
const char *to_string(const HttpResponseWriteErrorCode code) noexcept
{
    switch (code)
    {
    case HttpResponseWriteErrorCode::network_failure:
        return "HTTP response transmission failed";
    case HttpResponseWriteErrorCode::connection_closed:
        return "HTTP response connection made no write progress";
    case HttpResponseWriteErrorCode::body_source_failure:
        return "HTTP response body source failed";
    case HttpResponseWriteErrorCode::body_source_exception:
        return "HTTP response body source raised an exception";
    case HttpResponseWriteErrorCode::body_source_contract_violation:
        return "HTTP response body source violated its chunk contract";
    case HttpResponseWriteErrorCode::body_ended_early:
        return "HTTP response body ended before Content-Length";
    case HttpResponseWriteErrorCode::resource_allocation_failed:
        return "HTTP response writer could not allocate bounded state";
    }
    return "unknown HTTP response write error";
}

} // namespace sparenode::http
