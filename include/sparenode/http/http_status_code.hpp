#pragma once

#include <cstdint>

namespace sparenode::http
{

/// @brief Identifies standard HTTP response status codes supported by HTTP/1.1 semantics.
enum class HttpStatusCode : std::uint16_t
{
    continue_request = 100,                ///< Continue sending the request.
    switching_protocols = 101,             ///< Switch to the negotiated protocol.
    processing = 102,                      ///< WebDAV request processing continues.
    early_hints = 103,                     ///< Preliminary response headers follow.
    ok = 200,                              ///< Request completed successfully.
    created = 201,                         ///< A resource was created.
    accepted = 202,                        ///< Request was accepted for processing.
    non_authoritative_information = 203,   ///< Metadata came from another source.
    no_content = 204,                      ///< Success without a response body.
    reset_content = 205,                   ///< Client should reset its document view.
    partial_content = 206,                 ///< Response contains a requested byte range.
    multi_status = 207,                    ///< WebDAV response contains multiple statuses.
    already_reported = 208,                ///< WebDAV members were already reported.
    im_used = 226,                         ///< Instance manipulations were applied.
    multiple_choices = 300,                ///< Multiple representations are available.
    moved_permanently = 301,               ///< Resource has a permanent new URI.
    found = 302,                           ///< Resource is temporarily elsewhere.
    see_other = 303,                       ///< Retrieve the result from another URI.
    not_modified = 304,                    ///< Cached representation remains valid.
    use_proxy = 305,                       ///< Deprecated proxy redirection status.
    temporary_redirect = 307,              ///< Repeat the request at a temporary URI.
    permanent_redirect = 308,              ///< Repeat the request at a permanent URI.
    bad_request = 400,                     ///< Request syntax or framing is invalid.
    unauthorized = 401,                    ///< Authentication is required.
    payment_required = 402,                ///< Reserved for payment-related use.
    forbidden = 403,                       ///< Server refuses the request.
    not_found = 404,                       ///< Requested resource does not exist.
    method_not_allowed = 405,              ///< Method is not supported by the resource.
    not_acceptable = 406,                  ///< No acceptable representation is available.
    proxy_authentication_required = 407,   ///< Proxy authentication is required.
    request_timeout = 408,                 ///< Request was not completed in time.
    conflict = 409,                        ///< Request conflicts with resource state.
    gone = 410,                            ///< Resource is permanently unavailable.
    length_required = 411,                 ///< Content-Length is required.
    precondition_failed = 412,             ///< A request precondition evaluated false.
    content_too_large = 413,               ///< Request content exceeds server limits.
    uri_too_long = 414,                    ///< Request target exceeds server limits.
    unsupported_media_type = 415,          ///< Content format is unsupported.
    range_not_satisfiable = 416,           ///< Requested range cannot be served.
    expectation_failed = 417,              ///< Request expectation cannot be met.
    im_a_teapot = 418,                     ///< Reserved historical status code.
    misdirected_request = 421,             ///< Request reached the wrong server.
    unprocessable_content = 422,           ///< Content is syntactically valid but invalid.
    locked = 423,                          ///< WebDAV resource is locked.
    failed_dependency = 424,               ///< WebDAV dependency failed.
    too_early = 425,                       ///< Server rejects a replay-prone request.
    upgrade_required = 426,                ///< Client must switch protocols.
    precondition_required = 428,           ///< Server requires a conditional request.
    too_many_requests = 429,               ///< Client exceeded a request rate limit.
    request_header_fields_too_large = 431, ///< Request headers exceed server limits.
    unavailable_for_legal_reasons = 451,   ///< Legal restrictions block the resource.
    internal_server_error = 500,           ///< Server encountered an unexpected failure.
    not_implemented = 501,                 ///< Server does not implement the capability.
    bad_gateway = 502,                     ///< Upstream server returned an invalid response.
    service_unavailable = 503,             ///< Server is temporarily unavailable.
    gateway_timeout = 504,                 ///< Upstream server did not respond in time.
    http_version_not_supported = 505,      ///< HTTP version is unsupported.
    variant_also_negotiates = 506,         ///< Content negotiation is circular.
    insufficient_storage = 507,            ///< WebDAV storage is insufficient.
    loop_detected = 508,                   ///< WebDAV operation encountered a loop.
    not_extended = 510,                    ///< Further request extensions are required.
    network_authentication_required = 511  ///< Network authentication is required.
};

/// @brief Returns the three-digit wire representation of a typed HTTP status.
/// @param[in] status Typed HTTP response status.
/// @return Numeric status value used by HTTP serialization.
[[nodiscard]] constexpr std::uint16_t http_status_code_value(const HttpStatusCode status) noexcept
{
    return static_cast<std::uint16_t>(status);
}

} // namespace sparenode::http
