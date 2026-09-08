#pragma once

#include "sparenode/http/http_request_parser.hpp"
#include <memory>
#include <string>

namespace sparenode::http::detail
{
/// @brief Carries normalized routing bytes and the authority from an absolute target.
struct ParsedRequestTarget
{
    std::string_view routing_target{}; ///< Origin-form path/query or the server-wide asterisk.
    std::string_view authority{};      ///< Absolute-form authority, otherwise empty.
    std::shared_ptr<const std::string> storage{}; ///< Optional synthetic slash/query owner.
};

/// @brief Validates supported target forms without decoding or filesystem normalization.
/// @param[in] target Bounded request-target bytes borrowed from the request line.
/// @param[in] method Validated method; only OPTIONS may use asterisk-form.
/// @param[in] offset Absolute diagnostic offset of the request-target.
/// @return Routing representation with retained synthetic storage, or a target error.
[[nodiscard]] Result<ParsedRequestTarget, HttpRequestParseError>
parse_request_target(std::string_view target, HttpMethod method, std::size_t offset);

/// @brief Validates HTTP-version grammar and the supported HTTP/1.1-or-newer-minor policy.
/// @param[in] version Complete version token, including the case-sensitive HTTP prefix.
/// @param[in] offset Absolute diagnostic offset of that token.
/// @return Success, malformed version (400), or unsupported version (505).
[[nodiscard]] Result<void, HttpRequestParseError>
validate_request_version(std::string_view version, std::size_t offset) noexcept;
} // namespace sparenode::http::detail
