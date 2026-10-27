#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

#include "sparenode/http/http_application.hpp"
#include "sparenode/http/request/http_request_parser.hpp"
#include "sparenode/http/request/http_request_timeouts.hpp"
#include "sparenode/http/request/request_body_storage_policy.hpp"
#include "sparenode/http/transport/http_transport_policy.hpp"
#include "sparenode/http/transport/request_body_ingestor.hpp"
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
/// Returned deadlines may shorten but never extend configured receive budgets.
/// A missing deadline retains the configured limits. Implementations are invoked on a
/// dispatcher worker and must be safe for concurrent calls.
using HttpRequestDeadlineProvider = std::function<std::optional<network::NetworkDeadline>(
    HttpRequestReadPhase phase, network::NetworkDeadline connection_started)>;

/// @brief Defines bounded metadata, streaming-body, and I/O policy for one HTTP connection.
struct HttpConnectionHandlerConfig
{
    HttpRequestParserLimits parser_limits{}; ///< Protocol and request-size boundaries.
    storage::TemporaryFileFactory
        temporary_body_file_factory{}; ///< Optional controlled storage factory.
    RequestBodyStoragePolicySelector
        body_storage_policy_selector{}; ///< Optional metadata-driven body storage policy.
    std::size_t receive_chunk_bytes{std::size_t{16} * 1024}; ///< Maximum bytes per receive.
    HttpRequestTimeouts timeouts{}; ///< Header/body inactivity and total receive budgets.
    HttpRequestDeadlineProvider deadline_provider;       ///< Optional earlier per-read deadline.
    std::size_t max_drain_bytes{std::size_t{64} * 1024}; ///< Maximum extra socket bytes discarded.
    std::chrono::milliseconds drain_timeout{100};        ///< Absolute post-response drain budget.
    HttpTransportPolicy transport_policy{}; ///< Direct/LAN/proxy request trust boundary.
};

/// @brief Handles exactly one HTTP/1.1 request on an exclusively owned connection.
///
/// Metadata is accumulated incrementally in bounded memory while decoded body bytes
/// stream into an automatically cleaned temporary file. Any trailing pipelined bytes
/// remain untouched until response completion. Output is then half-closed and extra socket input
/// is discarded within the configured byte and absolute time limits. Parser failures receive one
/// bounded HTTP error response when the socket remains writable.
/// @param[in] connection Open connection transferred exclusively to this call.
/// @param[in] application Transport-neutral application shared by dispatcher workers.
/// @param[in] stop_token Worker cancellation token.
/// @param[in] config Parser, storage, and deadline policy.
/// @return Success after one response and EOF/drain budget, or orderly peer shutdown; otherwise the
/// structured network failure that terminated the connection.
[[nodiscard]] Result<void, network::NetworkError>
handle_http_connection(network::TcpConnection connection, const HttpApplication &application,
                       const std::stop_token &stop_token,
                       const HttpConnectionHandlerConfig &config = {});

/// @brief Creates a copyable dispatcher callback that shares one HTTP application.
/// @param[in] application Transport-neutral callback retained for every connection.
/// @param[in] config Bounded connection settings copied into the callback.
/// @return Connection handler suitable for `RunningApplication::start()`.
[[nodiscard]] network::ConnectionHandler
make_http_connection_handler(HttpApplication application, HttpConnectionHandlerConfig config = {});

/// @brief Creates the bounded header-ingestion gate paired with a connection config.
/// @param[in] config Parser and receive deadlines used by request workers.
/// @return Admission settings that release complete, oversized, or expired prefixes.
[[nodiscard]] network::ConnectionAdmissionOptions
make_http_connection_admission(const HttpConnectionHandlerConfig &config = {});

} // namespace sparenode::http
