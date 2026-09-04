#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

#include "sparenode/http/http_request_parser.hpp"
#include "sparenode/http/http_router.hpp"
#include "sparenode/network/connection_dispatcher.hpp"
#include "sparenode/network/network_io_options.hpp"

namespace sparenode::http
{

/// @brief Identifies which incomplete request section is awaiting more bytes.
enum class HttpRequestReadPhase : std::uint8_t
{
    headers, ///< Request line or header fields remain incomplete.
    body     ///< A complete header section declared body bytes that remain incomplete.
};

/// @brief Supplies an optional absolute deadline for each blocking request read.
///
/// SN-089 can provide phase-specific inactivity deadlines without coupling HTTP
/// configuration policy to the socket layer. Implementations are invoked on a
/// dispatcher worker and must be safe for concurrent calls.
using HttpRequestDeadlineProvider = std::function<std::optional<network::NetworkDeadline>(
    HttpRequestReadPhase phase, network::NetworkDeadline session_started)>;

/// @brief Defines bounded storage and injected I/O policy for one HTTP connection.
struct HttpConnectionHandlerConfig
{
    HttpRequestParserLimits parser_limits{}; ///< Protocol and request-size boundaries.
    std::size_t receive_chunk_bytes{std::size_t{16} * 1024}; ///< Maximum bytes per receive.
    HttpRequestDeadlineProvider deadline_provider;           ///< Optional per-read deadline policy.
};

/// @brief Handles exactly one HTTP/1.1 request on an exclusively owned connection.
///
/// Input is accumulated incrementally in bounded storage until the parser returns
/// one complete borrowed request. Any trailing pipelined bytes remain untouched
/// until response completion, then are discarded when this MVP session closes the
/// connection. Parser failures receive one bounded HTTP error response when the
/// socket remains writable.
/// @param[in] connection Open connection transferred exclusively to this call.
/// @param[in] router Immutable route table shared by dispatcher workers.
/// @param[in] stop_token Worker cancellation token.
/// @param[in] config Parser, storage, and deadline policy.
/// @return Success after one response or orderly peer shutdown; otherwise the
/// structured network failure that terminated the session.
[[nodiscard]] Result<void, network::NetworkError>
handle_http_connection(network::TcpConnection connection, const HttpRouter &router,
                       const std::stop_token &stop_token,
                       const HttpConnectionHandlerConfig &config = {});

/// @brief Creates a copyable dispatcher callback that shares one immutable router.
/// @param[in] router Route table retained for every concurrent session.
/// @param[in] config Bounded session settings copied into the callback.
/// @return Connection handler suitable for `RunningApplication::start()`.
[[nodiscard]] network::ConnectionHandler
make_http_connection_handler(std::shared_ptr<const HttpRouter> router,
                             HttpConnectionHandlerConfig config = {});

} // namespace sparenode::http
