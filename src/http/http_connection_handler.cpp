#include "sparenode/http/http_connection_handler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "sparenode/http/http_response_writer.hpp"

namespace sparenode::http
{
namespace
{

/// @brief Identifies internal session failures represented by the network handler contract.
enum class HttpSessionFailureCode : std::uint8_t
{
    unknown = 0,       ///< No specific internal session failure was identified.
    invalid_config,    ///< A zero or overflowing storage boundary was supplied.
    resource_failure,  ///< Bounded request storage could not be allocated.
    response_failure,  ///< A session-generated response violated its invariant.
    internal_exception ///< An injected policy or route unexpectedly threw.
};

/// Internal session details occupy 1 through 4; response-writer details occupy 100 through 106.
constexpr int response_writer_error_detail_base = 100;

/// @brief Groups immutable deadline state shared by every read in one session.
struct RequestDeadlineContext
{
    network::NetworkDeadline session_started;   ///< Monotonic start supplied to custom policies.
    network::NetworkDeadline fallback_deadline; ///< Validated total-request deadline.
};

/// @brief Converts one internal failure to the numeric network detail field.
[[nodiscard]] constexpr int error_detail(const HttpSessionFailureCode code) noexcept
{
    return static_cast<int>(code);
}

/// @brief Computes the largest request the configured parser can complete.
/// @param[in] limits Independent parser boundaries.
/// @return Combined byte bound, or zero when the addition would overflow.
[[nodiscard]] std::size_t maximum_request_bytes(const HttpRequestParserLimits &limits) noexcept
{
    constexpr std::size_t request_line_ending_bytes = 2;
    constexpr std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
    if (limits.max_request_line_bytes > maximum - request_line_ending_bytes)
    {
        return 0;
    }
    const std::size_t line_and_ending = limits.max_request_line_bytes + request_line_ending_bytes;
    if (limits.max_header_bytes > maximum - line_and_ending)
    {
        return 0;
    }
    const std::size_t metadata_bytes = line_and_ending + limits.max_header_bytes;
    if (limits.max_body_bytes > maximum - metadata_bytes)
    {
        return 0;
    }
    return metadata_bytes + limits.max_body_bytes;
}

/// @brief Detects whether the terminating empty header line has arrived.
/// @param[in] input Bytes accumulated for the current request.
/// @return Header or body phase for the next receive operation.
[[nodiscard]] HttpRequestReadPhase read_phase(const std::span<const std::byte> input) noexcept
{
    constexpr std::array terminator{std::byte{'\r'}, std::byte{'\n'}, std::byte{'\r'},
                                    std::byte{'\n'}};
    return std::ranges::search(input, terminator).empty() ? HttpRequestReadPhase::headers
                                                          : HttpRequestReadPhase::body;
}

/// @brief Forms a fallback deadline without overflowing duration conversion or time-point addition.
/// @param[in] session_started Monotonic start of the current session.
/// @param[in] timeout Positive total request budget expressed in milliseconds.
/// @return Absolute deadline, or no value when the timeout cannot be represented safely.
[[nodiscard]] std::optional<network::NetworkDeadline>
request_deadline(const network::NetworkDeadline session_started,
                 const std::chrono::milliseconds timeout) noexcept
{
    using DeadlineDuration = network::NetworkDeadline::duration;

    if (timeout.count() <= 0 || session_started < network::NetworkDeadline{})
    {
        return std::nullopt;
    }

    const DeadlineDuration remaining =
        network::NetworkDeadline::max().time_since_epoch() - session_started.time_since_epoch();
    const auto maximum_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (timeout > maximum_timeout)
    {
        return std::nullopt;
    }

    const DeadlineDuration converted_timeout =
        std::chrono::duration_cast<DeadlineDuration>(timeout);
    if (converted_timeout <= DeadlineDuration::zero() || converted_timeout > remaining)
    {
        return std::nullopt;
    }
    return session_started + converted_timeout;
}

/// @brief Selects the standard response status for one parser failure.
/// @param[in] code Parser category to map without exposing request data.
/// @return Bounded client-error status.
[[nodiscard]] HttpStatusCode parse_error_status(const HttpRequestParseErrorCode code) noexcept
{
    switch (code)
    {
    case HttpRequestParseErrorCode::request_line_too_large:
        return HttpStatusCode::uri_too_long;
    case HttpRequestParseErrorCode::headers_too_large:
    case HttpRequestParseErrorCode::too_many_headers:
        return HttpStatusCode::request_header_fields_too_large;
    case HttpRequestParseErrorCode::body_too_large:
        return HttpStatusCode::content_too_large;
    case HttpRequestParseErrorCode::unsupported_http_version:
        return HttpStatusCode::http_version_not_supported;
    case HttpRequestParseErrorCode::unsupported_method:
    case HttpRequestParseErrorCode::unsupported_transfer_encoding:
        return HttpStatusCode::not_implemented;
    case HttpRequestParseErrorCode::invalid_line_ending:
    case HttpRequestParseErrorCode::malformed_request_line:
    case HttpRequestParseErrorCode::invalid_request_target:
    case HttpRequestParseErrorCode::invalid_method:
    case HttpRequestParseErrorCode::malformed_header:
    case HttpRequestParseErrorCode::folded_header:
    case HttpRequestParseErrorCode::missing_host:
    case HttpRequestParseErrorCode::duplicate_host:
    case HttpRequestParseErrorCode::invalid_host:
    case HttpRequestParseErrorCode::invalid_content_length:
    case HttpRequestParseErrorCode::duplicate_content_length:
        return HttpStatusCode::bad_request;
    }
    return HttpStatusCode::bad_request;
}

/// @brief Returns the canonical reason phrase for a session-generated status.
/// @param[in] status Status selected by parsing or routing failure handling.
/// @return Static ASCII reason phrase.
[[nodiscard]] std::string_view reason_phrase(const HttpStatusCode status) noexcept
{
    switch (status)
    {
    case HttpStatusCode::bad_request:
        return "Bad Request";
    case HttpStatusCode::content_too_large:
        return "Content Too Large";
    case HttpStatusCode::uri_too_long:
        return "URI Too Long";
    case HttpStatusCode::request_header_fields_too_large:
        return "Request Header Fields Too Large";
    case HttpStatusCode::internal_server_error:
        return "Internal Server Error";
    case HttpStatusCode::not_implemented:
        return "Not Implemented";
    case HttpStatusCode::http_version_not_supported:
        return "HTTP Version Not Supported";
    default:
        return "Error";
    }
}

/// @brief Creates one empty response for a session-owned failure boundary.
/// @param[in] status Standard failure status.
/// @return Valid bounded response or a stable internal session error.
[[nodiscard]] Result<HttpResponse, network::NetworkError>
make_error_response(const HttpStatusCode status)
{
    auto response = HttpResponse::create(status, std::string(reason_phrase(status)),
                                         {{"Connection", "close"}}, {});
    if (!response)
    {
        return unexpected(network::NetworkError{
            network::NetworkOperation::send, network::NetworkErrorDomain::state,
            error_detail(HttpSessionFailureCode::response_failure)});
    }
    return std::move(response).value();
}

/// @brief Converts a response-writer failure to the dispatcher network contract.
/// @param[in] error Structured HTTP writer failure.
/// @return Nested network failure when available, otherwise a stable state error.
[[nodiscard]] network::NetworkError map_write_error(const HttpResponseWriteError &error) noexcept
{
    if (error.network_error.has_value())
    {
        return error.network_error.value();
    }
    return {network::NetworkOperation::send, network::NetworkErrorDomain::state,
            response_writer_error_detail_base + static_cast<int>(error.code)};
}

/// @brief Sends one session-owned error response.
/// @param[in,out] connection Exclusive connection used for transmission.
/// @param[in] status Error status sent with an empty body.
/// @param[in] stop_token Cancellation token observed during writes.
/// @return Success or a mapped response-construction/write failure.
[[nodiscard]] Result<void, network::NetworkError>
send_error_response(network::TcpConnection &connection, const HttpStatusCode status,
                    const std::stop_token &stop_token)
{
    auto response = make_error_response(status);
    if (!response)
    {
        return unexpected(response.error());
    }
    if (auto written = write_http_response(connection, response.value(), stop_token); !written)
    {
        return unexpected(map_write_error(written.error()));
    }
    return {};
}

/// @brief Resolves cancellation and deadline options for the next request read.
/// @param[in] config Session configuration and optional provider.
/// @param[in] phase Incomplete request section awaiting input.
/// @param[in] deadlines Validated session start and fallback deadline.
/// @param[in] stop_token Dispatcher cancellation token.
/// @return Provider result or the bounded total-request fallback deadline.
[[nodiscard]] network::NetworkIoOptions read_options(const HttpConnectionHandlerConfig &config,
                                                     const HttpRequestReadPhase phase,
                                                     const RequestDeadlineContext &deadlines,
                                                     const std::stop_token &stop_token)
{
    if (config.deadline_provider)
    {
        const auto provided_deadline = config.deadline_provider(phase, deadlines.session_started);
        if (provided_deadline.has_value())
        {
            return {.stop_token = stop_token, .deadline = provided_deadline};
        }
    }
    return {.stop_token = stop_token, .deadline = deadlines.fallback_deadline};
}

/// @brief Dispatches one complete request and writes its response.
/// @param[in,out] connection Exclusive connection used for transmission.
/// @param[in] router Immutable route table used for dispatch.
/// @param[in] request Complete request whose views borrow the session buffer.
/// @param[in] stop_token Cancellation token observed during writes.
/// @return Success or a mapped routing/write failure.
[[nodiscard]] Result<void, network::NetworkError>
dispatch_and_respond(network::TcpConnection &connection, const HttpRouter &router,
                     const HttpRequestView &request, const std::stop_token &stop_token)
{
    auto response = router.dispatch(request);
    if (!response)
    {
        return send_error_response(connection, HttpStatusCode::internal_server_error, stop_token);
    }
    if (auto written = write_http_response(connection, response.value(), stop_token); !written)
    {
        return unexpected(map_write_error(written.error()));
    }
    return {};
}

} // namespace

Result<void, network::NetworkError>
handle_http_connection(network::TcpConnection connection, const HttpRouter &router,
                       const std::stop_token &stop_token, const HttpConnectionHandlerConfig &config)
{
    const std::size_t request_limit = maximum_request_bytes(config.parser_limits);
    const auto session_started = std::chrono::steady_clock::now();
    const auto fallback_deadline = request_deadline(session_started, config.request_timeout);
    if (request_limit == 0 || config.receive_chunk_bytes == 0 || !fallback_deadline.has_value())
    {
        return unexpected(network::NetworkError{
            network::NetworkOperation::receive, network::NetworkErrorDomain::validation,
            error_detail(HttpSessionFailureCode::invalid_config)});
    }
    const RequestDeadlineContext deadlines{session_started, fallback_deadline.value()};

    try
    {
        std::vector<std::byte> input;
        input.reserve((std::min)(request_limit, config.receive_chunk_bytes));
        while (true)
        {
            auto parsed = parse_http_request(input, config.parser_limits);
            if (!parsed)
            {
                return send_error_response(connection, parse_error_status(parsed.error().code),
                                           stop_token);
            }
            const auto &request = parsed->request();
            if (request.has_value())
            {
                return dispatch_and_respond(connection, router, request.value(), stop_token);
            }
            if (input.size() == request_limit)
            {
                return send_error_response(connection, HttpStatusCode::bad_request, stop_token);
            }

            const HttpRequestReadPhase phase = read_phase(input);
            const std::size_t available = request_limit - input.size();
            const std::size_t requested = (std::min)(available, config.receive_chunk_bytes);
            const std::size_t previous_size = input.size();
            input.resize(previous_size + requested);
            auto received =
                connection.receive_with_options(std::span(input).subspan(previous_size),
                                                read_options(config, phase, deadlines, stop_token));
            if (!received)
            {
                return unexpected(received.error());
            }
            if (received.value() == 0)
            {
                return {};
            }
            input.resize(previous_size + received.value());
        }
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(network::NetworkError{
            network::NetworkOperation::receive, network::NetworkErrorDomain::state,
            error_detail(HttpSessionFailureCode::resource_failure)});
    }
    catch (...)
    {
        return unexpected(network::NetworkError{
            network::NetworkOperation::receive, network::NetworkErrorDomain::state,
            error_detail(HttpSessionFailureCode::internal_exception)});
    }
}

network::ConnectionHandler make_http_connection_handler(std::shared_ptr<const HttpRouter> router,
                                                        HttpConnectionHandlerConfig config)
{
    return [router = std::move(router), config = std::move(config)](
               network::TcpConnection connection,
               const std::stop_token &stop_token) -> Result<void, network::NetworkError>
    {
        if (router == nullptr)
        {
            return unexpected(network::NetworkError{
                network::NetworkOperation::receive, network::NetworkErrorDomain::validation,
                error_detail(HttpSessionFailureCode::invalid_config)});
        }
        return handle_http_connection(std::move(connection), *router, stop_token, config);
    };
}

} // namespace sparenode::http
