#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

#include "sparenode/http/http_request.hpp"
#include "sparenode/http/http_response.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Identifies why a validated HTTP response could not be transmitted completely.
enum class HttpResponseWriteErrorCode : std::uint8_t
{
    network_failure,                ///< TCP transmission returned a structured error.
    connection_closed,              ///< A non-empty send made no progress.
    body_source_failure,            ///< The streaming body reader returned an error.
    body_source_exception,          ///< The streaming body reader unexpectedly threw.
    body_source_contract_violation, ///< The reader reported more bytes than its destination.
    body_ended_early,               ///< The reader ended before declared Content-Length.
    resource_allocation_failed      ///< Response-head or stream-buffer allocation failed.
};

/// @brief Preserves the response-write boundary and its optional nested failure.
struct HttpResponseWriteError
{
    HttpResponseWriteErrorCode code{};                  ///< Stable transport failure category.
    std::optional<network::NetworkError> network_error; ///< Nested TCP failure when present.
    std::optional<HttpBodyReadError> body_error;        ///< Nested body failure when present.
};

/// @brief Serializes validated framing and Connection: close for every final response.
/// @param[in] response Response whose body is not consumed.
/// @return Complete HTTP/1.1 head ending in CRLF CRLF.
[[nodiscard]] std::string serialize_http_response_head(const HttpResponse &response);

/// @brief Sends one complete validated response with bounded intermediate memory.
///
/// Partial native writes are retried until the selected response bytes are sent.
/// HEAD sends only the head and leaves both memory and streaming bodies untouched.
/// Other methods consume a streaming reader in fixed-size chunks up to Content-Length.
/// @param[in,out] connection Exclusive connection used for the complete transmission.
/// @param[in,out] response Response whose streaming reader advances during the call.
/// @param[in] stop_token Token observed by network and body-source operations.
/// @param[in] request_method HEAD transmits only metadata and never advances a body reader.
/// @return Success after complete delivery, or a structured terminal failure.
[[nodiscard]] Result<void, HttpResponseWriteError>
write_http_response(network::TcpConnection &connection, HttpResponse &response,
                    const std::stop_token &stop_token = {},
                    HttpMethod request_method = HttpMethod::get);

/// @brief Returns stable text for one response transmission failure.
[[nodiscard]] const char *to_string(HttpResponseWriteErrorCode code) noexcept;

} // namespace sparenode::http
