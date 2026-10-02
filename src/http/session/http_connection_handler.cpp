#include "sparenode/http/session/http_connection_handler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "sparenode/http/response/http_response_writer.hpp"
#include "sparenode/http/session/detail/buffered_request.hpp"
#include "sparenode/http/session/detail/request_deadlines.hpp"

namespace sparenode::http
{
namespace
{

/// @brief Identifies internal session failures represented by the network handler contract.
enum class HttpSessionFailureCode : std::uint8_t
{
    unknown = 0,       ///< No specific internal session failure was identified.
    invalid_config,    ///< An invalid storage boundary or receive budget was supplied.
    resource_failure,  ///< Bounded request storage could not be allocated.
    response_failure,  ///< A session-generated response violated its invariant.
    internal_exception ///< An injected policy or route unexpectedly threw.
};

/// Internal session details occupy 1 through 4; response-writer details occupy 100 through 106.
constexpr int response_writer_error_detail_base = 100;

/// @brief Converts one internal failure to the numeric network detail field.
[[nodiscard]] constexpr int error_detail(const HttpSessionFailureCode code) noexcept
{
    return static_cast<int>(code);
}

/// @brief Computes the largest metadata prefix retained by the session.
/// @param[in] limits Independent parser boundaries.
/// @return Request-line plus header bound, or zero when the addition would overflow.
[[nodiscard]] std::size_t maximum_metadata_bytes(const HttpRequestParserLimits &limits) noexcept
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
    return line_and_ending + limits.max_header_bytes;
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
    case HttpRequestParseErrorCode::chunk_metadata_too_large:
    case HttpRequestParseErrorCode::trailers_too_large:
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
    case HttpRequestParseErrorCode::invalid_transfer_encoding:
    case HttpRequestParseErrorCode::conflicting_message_length:
    case HttpRequestParseErrorCode::malformed_chunk:
    case HttpRequestParseErrorCode::invalid_trailer:
    case HttpRequestParseErrorCode::incomplete_body:
    case HttpRequestParseErrorCode::malformed_http_version:
        return HttpStatusCode::bad_request;
    }
    return HttpStatusCode::bad_request;
}

/// @brief Reports whether request ingestion stopped because the caller cancelled it.
[[nodiscard]] bool cancelled(const detail::BufferedRequestError &error) noexcept
{
    const auto *ingestion = std::get_if<RequestBodyIngestionError>(&error);
    return ingestion != nullptr && ingestion->code == RequestBodyIngestionErrorCode::cancelled;
}

/// @brief Reports whether body persistence crossed the active receive deadline.
[[nodiscard]] bool timed_out(const detail::BufferedRequestError &error) noexcept
{
    const auto *ingestion = std::get_if<RequestBodyIngestionError>(&error);
    return ingestion != nullptr &&
           ingestion->code == RequestBodyIngestionErrorCode::deadline_exceeded;
}

/// @brief Maps metadata and body-ingestion failures to a bounded HTTP status.
[[nodiscard]] HttpStatusCode
request_error_status(const detail::BufferedRequestError &error) noexcept
{
    if (const auto *parse = std::get_if<HttpRequestParseError>(&error))
    {
        return parse_error_status(parse->code);
    }
    const auto *ingestion = std::get_if<RequestBodyIngestionError>(&error);
    if (ingestion == nullptr)
    {
        return HttpStatusCode::internal_server_error;
    }
    switch (ingestion->code)
    {
    case RequestBodyIngestionErrorCode::body_too_large:
        return HttpStatusCode::content_too_large;
    case RequestBodyIngestionErrorCode::framing_failure:
        return parse_error_status(ingestion->framing_error.code);
    case RequestBodyIngestionErrorCode::temporary_file_failure:
    case RequestBodyIngestionErrorCode::resource_failure:
    case RequestBodyIngestionErrorCode::cancelled:
    case RequestBodyIngestionErrorCode::deadline_exceeded:
        return HttpStatusCode::internal_server_error;
    }
    return HttpStatusCode::internal_server_error;
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
    case HttpStatusCode::expectation_failed:
        return "Expectation Failed";
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
    auto response = HttpResponse::create(status, std::string(reason_phrase(status)), {}, {});
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

/// @brief Half-closes output and discards peer input within fixed byte and time budgets.
/// @param[in,out] connection Connection whose final response has already been sent.
/// @param[in] stop_token Cancellation observed even while input remains continuously readable.
/// @param[in] config Bounded close policy independent of request receive deadlines.
/// @return Success at EOF or a budget boundary; cancellation/native failures remain observable.
[[nodiscard]] Result<void, network::NetworkError>
drain_after_response(network::TcpConnection &connection, const std::stop_token &stop_token,
                     const HttpConnectionHandlerConfig &config)
{
    if (stop_token.stop_requested())
    {
        return unexpected(network::NetworkError{network::NetworkOperation::receive,
                                                network::NetworkErrorDomain::cancellation, 0});
    }
    if (auto shutdown = connection.shutdown_send(); !shutdown)
    {
        return shutdown;
    }
    const auto deadline = std::chrono::steady_clock::now() + config.drain_timeout;
    constexpr std::size_t drain_buffer_bytes = 4096;
    std::array<std::byte, drain_buffer_bytes> buffer{};
    std::size_t remaining = config.max_drain_bytes;
    while (remaining > 0)
    {
        if (stop_token.stop_requested())
        {
            return unexpected(network::NetworkError{network::NetworkOperation::receive,
                                                    network::NetworkErrorDomain::cancellation, 0});
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return {};
        }
        auto received = connection.receive_with_options(
            std::span(buffer).first((std::min)(remaining, buffer.size())),
            {.stop_token = stop_token, .deadline = deadline});
        if (!received)
        {
            if (received.error().domain == network::NetworkErrorDomain::timeout)
            {
                return {};
            }
            return unexpected(received.error());
        }
        if (received.value() == 0)
        {
            return {};
        }
        remaining -= received.value();
    }
    return {};
}

/// @brief Sends one session-owned error response.
/// @param[in,out] connection Exclusive connection used for transmission.
/// @param[in] status Error status sent with an empty body.
/// @param[in] stop_token Cancellation token observed during writes.
/// @param[in] config Bounded post-response drain policy.
/// @return Success or a mapped response-construction/write failure.
[[nodiscard]] Result<void, network::NetworkError>
send_error_response(network::TcpConnection &connection, const HttpStatusCode status,
                    const std::stop_token &stop_token, const HttpConnectionHandlerConfig &config)
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
    return drain_after_response(connection, stop_token, config);
}

/// @brief Sends one interim continue or an immediate final expectation rejection.
/// @param[in,out] connection Session-owned transport.
/// @param[in] expectation One-time decision from validated headers.
/// @param[in] options Session cancellation and built-in body/total deadline.
/// @param[in] config Final response close policy.
/// @return True to continue request ingestion, false after a final response, or a write error.
[[nodiscard]] Result<bool, network::NetworkError> respond_to_expectation(
    network::TcpConnection &connection, const detail::RequestExpectation expectation,
    const network::NetworkIoOptions &options, const HttpConnectionHandlerConfig &config)
{
    if (expectation == detail::RequestExpectation::unsupported)
    {
        const auto sent = send_error_response(connection, HttpStatusCode::expectation_failed,
                                              options.stop_token, config);
        if (!sent)
        {
            return unexpected(sent.error());
        }
        return false;
    }
    if (expectation == detail::RequestExpectation::continue_request)
    {
        constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";
        auto remaining = std::as_bytes(std::span(interim.data(), interim.size()));
        while (!remaining.empty())
        {
            const auto sent = connection.send_with_options(remaining, options);
            if (!sent)
            {
                return unexpected(sent.error());
            }
            if (sent.value() == 0)
            {
                return unexpected(network::NetworkError{
                    network::NetworkOperation::send, network::NetworkErrorDomain::state,
                    error_detail(HttpSessionFailureCode::response_failure)});
            }
            remaining = remaining.subspan(sent.value());
        }
    }
    return true;
}

/// @brief Resolves cancellation and deadline options for the next request read.
/// @param[in] config Session configuration and optional provider.
/// @param[in] phase Incomplete request section awaiting input.
/// @param[in] session_started Monotonic session start supplied to custom policies.
/// @param[in] stop_token Dispatcher cancellation token.
/// @return Optional custom deadline; built-in limits are applied by the receive loop.
[[nodiscard]] network::NetworkIoOptions read_options(const HttpConnectionHandlerConfig &config,
                                                     const HttpRequestReadPhase phase,
                                                     const network::NetworkDeadline session_started,
                                                     const std::stop_token &stop_token)
{
    return {.stop_token = stop_token,
            .deadline = config.deadline_provider ? config.deadline_provider(phase, session_started)
                                                 : std::nullopt};
}

/// @brief Copies the active receive policy into one temporary-file operation.
[[nodiscard]] filesystem::TemporaryFileIoOptions
file_options(const network::NetworkIoOptions &options) noexcept
{
    return {.stop_token = options.stop_token, .deadline = options.deadline};
}

/// @brief Dispatches one complete request and writes its response.
/// @param[in,out] connection Exclusive connection used for transmission.
/// @param[in] router Immutable route table used for dispatch.
/// @param[in] request Complete request whose views borrow the session buffer.
/// @param[in] stop_token Cancellation token observed during writes.
/// @param[in] config Bounded post-response drain policy.
/// @return Success or a mapped routing/write failure.
[[nodiscard]] Result<void, network::NetworkError>
dispatch_and_respond(network::TcpConnection &connection, const HttpRouter &router,
                     const HttpRequestView &request, const std::stop_token &stop_token,
                     const HttpConnectionHandlerConfig &config)
{
    auto response = router.dispatch(request);
    if (!response || http_status_code_value(response->status_code()) <
                         http_status_code_value(HttpStatusCode::ok))
    {
        return send_error_response(connection, HttpStatusCode::internal_server_error, stop_token,
                                   config);
    }
    if (auto written =
            write_http_response(connection, response.value(), stop_token, request.method());
        !written)
    {
        return unexpected(map_write_error(written.error()));
    }
    return drain_after_response(connection, stop_token, config);
}

/// @brief Groups validated state shared by one request receive loop.
struct HttpSessionRunContext
{
    /// @brief Retains references needed by the receive loop.
    HttpSessionRunContext(const HttpRouter &router_value, const std::stop_token &stop_token_value,
                          const HttpConnectionHandlerConfig &config_value,
                          detail::RequestDeadlines &deadlines_value,
                          const network::NetworkDeadline started_value) noexcept
        : router(router_value), stop_token(stop_token_value), config(config_value),
          deadlines(deadlines_value), started(started_value)
    {
    }

    const HttpRouter &router;                  ///< Immutable dispatch table.
    const std::stop_token &stop_token;         ///< Dispatcher cancellation source.
    const HttpConnectionHandlerConfig &config; ///< Validated session policy.
    detail::RequestDeadlines &deadlines;       ///< Mutable receive deadline state.
    network::NetworkDeadline started;          ///< Session start for injected policies.
};

/// @brief Combines the current phase deadline with the optional shorter provider deadline.
[[nodiscard]] network::NetworkIoOptions bounded_read_options(const HttpSessionRunContext &context,
                                                             const HttpRequestReadPhase phase)
{
    auto options = read_options(context.config, phase, context.started, context.stop_token);
    const auto bounded_deadline = context.deadlines.next(phase == HttpRequestReadPhase::body);
    options.deadline = options.deadline.has_value()
                           ? (std::min)(options.deadline.value(), bounded_deadline)
                           : bounded_deadline;
    return options;
}

/// @brief Converts a buffered-request failure into a network interruption or HTTP response.
[[nodiscard]] Result<void, network::NetworkError>
respond_to_request_failure(network::TcpConnection &connection,
                           const detail::BufferedRequestError &error,
                           const HttpSessionRunContext &context)
{
    if (cancelled(error))
    {
        return unexpected(network::NetworkError{network::NetworkOperation::receive,
                                                network::NetworkErrorDomain::cancellation, 0});
    }
    if (timed_out(error))
    {
        return unexpected(network::NetworkError{network::NetworkOperation::receive,
                                                network::NetworkErrorDomain::timeout, 0});
    }
    return send_error_response(connection, request_error_status(error), context.stop_token,
                               context.config);
}

/// @brief Runs one validated session while the public boundary contains exceptions.
[[nodiscard]] Result<void, network::NetworkError>
run_http_session(network::TcpConnection &connection, const HttpSessionRunContext &context)
{
    detail::BufferedRequest request(context.config.parser_limits,
                                    context.config.temporary_body_file_factory);
    std::vector<std::byte> input((std::min)(maximum_metadata_bytes(context.config.parser_limits),
                                            context.config.receive_chunk_bytes));
    while (true)
    {
        const auto expectation = respond_to_expectation(
            connection, request.take_expectation(),
            {.stop_token = context.stop_token, .deadline = context.deadlines.next(true)},
            context.config);
        if (!expectation)
        {
            return unexpected(expectation.error());
        }
        if (!expectation.value())
        {
            return {};
        }
        if (request.complete())
        {
            return dispatch_and_respond(connection, context.router, request.request(),
                                        context.stop_token, context.config);
        }
        const auto phase =
            request.reading_body() ? HttpRequestReadPhase::body : HttpRequestReadPhase::headers;
        const auto options = bounded_read_options(context, phase);
        auto received = connection.receive_with_options(input, options);
        if (!received)
        {
            return unexpected(received.error());
        }
        if (received.value() == 0)
        {
            if (request.empty())
            {
                return {};
            }
            const auto finished = request.finish(file_options(options));
            if (!finished)
            {
                return respond_to_request_failure(connection, finished.error(), context);
            }
            return send_error_response(connection, HttpStatusCode::bad_request, context.stop_token,
                                       context.config);
        }
        context.deadlines.record_progress(std::chrono::steady_clock::now());
        const auto parsed = request.feed_with_body_options(
            std::span(input).first(received.value()), [&context]
            { return file_options(bounded_read_options(context, HttpRequestReadPhase::body)); });
        if (!parsed)
        {
            return respond_to_request_failure(connection, parsed.error(), context);
        }
    }
}

} // namespace

/// @brief Receives one bounded request, then routes it and sends its response.
/// @return Success after response or peer EOF; otherwise a structured session failure.
/// @details Nonempty receives renew inactivity but never the total receive deadline.
/// Timeout returns immediately without attempting an HTTP error response.
Result<void, network::NetworkError>
handle_http_connection(network::TcpConnection connection, const HttpRouter &router,
                       const std::stop_token &stop_token, const HttpConnectionHandlerConfig &config)
{
    const std::size_t metadata_limit = maximum_metadata_bytes(config.parser_limits);
    const auto session_started = std::chrono::steady_clock::now();
    auto deadline_state = detail::RequestDeadlines::create(config.timeouts, session_started);
    if (metadata_limit == 0 || config.receive_chunk_bytes == 0 || !deadline_state.has_value() ||
        config.max_drain_bytes == 0 || config.drain_timeout <= std::chrono::milliseconds::zero() ||
        config.drain_timeout > HttpRequestTimeouts::maximum_config_timeout)
    {
        return unexpected(network::NetworkError{
            network::NetworkOperation::receive, network::NetworkErrorDomain::validation,
            error_detail(HttpSessionFailureCode::invalid_config)});
    }
    auto &deadlines = deadline_state.value();

    try
    {
        return run_http_session(connection,
                                {router, stop_token, config, deadlines, session_started});
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

/// @brief Binds immutable routing and timeout configuration into a connection handler.
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
