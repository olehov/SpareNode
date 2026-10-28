#include "sparenode/http/http_status_code.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace sparenode::http
{
namespace
{

/// Canonical reason phrases indexed through their non-contiguous typed status values.
constexpr std::array reason_phrases{
    std::pair{HttpStatusCode::continue_request, std::string_view{"Continue"}},
    std::pair{HttpStatusCode::switching_protocols, std::string_view{"Switching Protocols"}},
    std::pair{HttpStatusCode::processing, std::string_view{"Processing"}},
    std::pair{HttpStatusCode::early_hints, std::string_view{"Early Hints"}},
    std::pair{HttpStatusCode::ok, std::string_view{"OK"}},
    std::pair{HttpStatusCode::created, std::string_view{"Created"}},
    std::pair{HttpStatusCode::accepted, std::string_view{"Accepted"}},
    std::pair{HttpStatusCode::non_authoritative_information,
              std::string_view{"Non-Authoritative Information"}},
    std::pair{HttpStatusCode::no_content, std::string_view{"No Content"}},
    std::pair{HttpStatusCode::reset_content, std::string_view{"Reset Content"}},
    std::pair{HttpStatusCode::partial_content, std::string_view{"Partial Content"}},
    std::pair{HttpStatusCode::multi_status, std::string_view{"Multi-Status"}},
    std::pair{HttpStatusCode::already_reported, std::string_view{"Already Reported"}},
    std::pair{HttpStatusCode::im_used, std::string_view{"IM Used"}},
    std::pair{HttpStatusCode::multiple_choices, std::string_view{"Multiple Choices"}},
    std::pair{HttpStatusCode::moved_permanently, std::string_view{"Moved Permanently"}},
    std::pair{HttpStatusCode::found, std::string_view{"Found"}},
    std::pair{HttpStatusCode::see_other, std::string_view{"See Other"}},
    std::pair{HttpStatusCode::not_modified, std::string_view{"Not Modified"}},
    std::pair{HttpStatusCode::use_proxy, std::string_view{"Use Proxy"}},
    std::pair{HttpStatusCode::temporary_redirect, std::string_view{"Temporary Redirect"}},
    std::pair{HttpStatusCode::permanent_redirect, std::string_view{"Permanent Redirect"}},
    std::pair{HttpStatusCode::bad_request, std::string_view{"Bad Request"}},
    std::pair{HttpStatusCode::unauthorized, std::string_view{"Unauthorized"}},
    std::pair{HttpStatusCode::payment_required, std::string_view{"Payment Required"}},
    std::pair{HttpStatusCode::forbidden, std::string_view{"Forbidden"}},
    std::pair{HttpStatusCode::not_found, std::string_view{"Not Found"}},
    std::pair{HttpStatusCode::method_not_allowed, std::string_view{"Method Not Allowed"}},
    std::pair{HttpStatusCode::not_acceptable, std::string_view{"Not Acceptable"}},
    std::pair{HttpStatusCode::proxy_authentication_required,
              std::string_view{"Proxy Authentication Required"}},
    std::pair{HttpStatusCode::request_timeout, std::string_view{"Request Timeout"}},
    std::pair{HttpStatusCode::conflict, std::string_view{"Conflict"}},
    std::pair{HttpStatusCode::gone, std::string_view{"Gone"}},
    std::pair{HttpStatusCode::length_required, std::string_view{"Length Required"}},
    std::pair{HttpStatusCode::precondition_failed, std::string_view{"Precondition Failed"}},
    std::pair{HttpStatusCode::content_too_large, std::string_view{"Content Too Large"}},
    std::pair{HttpStatusCode::uri_too_long, std::string_view{"URI Too Long"}},
    std::pair{HttpStatusCode::unsupported_media_type, std::string_view{"Unsupported Media Type"}},
    std::pair{HttpStatusCode::range_not_satisfiable, std::string_view{"Range Not Satisfiable"}},
    std::pair{HttpStatusCode::expectation_failed, std::string_view{"Expectation Failed"}},
    std::pair{HttpStatusCode::im_a_teapot, std::string_view{"I'm a Teapot"}},
    std::pair{HttpStatusCode::misdirected_request, std::string_view{"Misdirected Request"}},
    std::pair{HttpStatusCode::unprocessable_content, std::string_view{"Unprocessable Content"}},
    std::pair{HttpStatusCode::locked, std::string_view{"Locked"}},
    std::pair{HttpStatusCode::failed_dependency, std::string_view{"Failed Dependency"}},
    std::pair{HttpStatusCode::too_early, std::string_view{"Too Early"}},
    std::pair{HttpStatusCode::upgrade_required, std::string_view{"Upgrade Required"}},
    std::pair{HttpStatusCode::precondition_required, std::string_view{"Precondition Required"}},
    std::pair{HttpStatusCode::too_many_requests, std::string_view{"Too Many Requests"}},
    std::pair{HttpStatusCode::request_header_fields_too_large,
              std::string_view{"Request Header Fields Too Large"}},
    std::pair{HttpStatusCode::unavailable_for_legal_reasons,
              std::string_view{"Unavailable For Legal Reasons"}},
    std::pair{HttpStatusCode::internal_server_error, std::string_view{"Internal Server Error"}},
    std::pair{HttpStatusCode::not_implemented, std::string_view{"Not Implemented"}},
    std::pair{HttpStatusCode::bad_gateway, std::string_view{"Bad Gateway"}},
    std::pair{HttpStatusCode::service_unavailable, std::string_view{"Service Unavailable"}},
    std::pair{HttpStatusCode::gateway_timeout, std::string_view{"Gateway Timeout"}},
    std::pair{HttpStatusCode::http_version_not_supported,
              std::string_view{"HTTP Version Not Supported"}},
    std::pair{HttpStatusCode::variant_also_negotiates, std::string_view{"Variant Also Negotiates"}},
    std::pair{HttpStatusCode::insufficient_storage, std::string_view{"Insufficient Storage"}},
    std::pair{HttpStatusCode::loop_detected, std::string_view{"Loop Detected"}},
    std::pair{HttpStatusCode::not_extended, std::string_view{"Not Extended"}},
    std::pair{HttpStatusCode::network_authentication_required,
              std::string_view{"Network Authentication Required"}},
};

static_assert(std::ranges::is_sorted(reason_phrases,
                                     [](const auto &left, const auto &right) {
                                         return http_status_code_value(left.first) <
                                                http_status_code_value(right.first);
                                     }));

} // namespace

std::string_view http_reason_phrase(const HttpStatusCode status) noexcept
{
    const auto mapping = std::lower_bound(
        reason_phrases.begin(), reason_phrases.end(), status,
        [](const auto &entry, const HttpStatusCode candidate)
        { return http_status_code_value(entry.first) < http_status_code_value(candidate); });
    if (mapping != reason_phrases.end() && mapping->first == status)
    {
        return mapping->second;
    }
    return "Unknown Status";
}

} // namespace sparenode::http
