#include "sparenode/http/http_request.hpp"

#include <algorithm>
#include <utility>

namespace sparenode::http
{
namespace
{

/// @brief Compares ASCII field names without locale-dependent case conversion.
[[nodiscard]] constexpr bool ascii_case_insensitive_equal(const std::string_view left,
                                                          const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const char left_byte = left[index];
        const char right_byte = right[index];
        const char normalized_left = left_byte >= 'A' && left_byte <= 'Z'
                                         ? static_cast<char>(left_byte + ('a' - 'A'))
                                         : left_byte;
        const char normalized_right = right_byte >= 'A' && right_byte <= 'Z'
                                          ? static_cast<char>(right_byte + ('a' - 'A'))
                                          : right_byte;
        if (normalized_left != normalized_right)
        {
            return false;
        }
    }
    return true;
}

} // namespace

/// @brief Stores parser-validated borrowed request components without exposing mutation.
HttpRequestView::HttpRequestView(const HttpMethod method, const std::string_view target,
                                 std::vector<HttpHeaderView> headers,
                                 const std::span<const std::byte> body)
    : method_(method), target_(target), headers_(std::move(headers)), body_(body)
{
}

/// @brief Returns the validated method stored by the parser.
HttpMethod HttpRequestView::method() const noexcept
{
    return method_;
}

/// @brief Returns the validated borrowed origin-form request target.
std::string_view HttpRequestView::target() const noexcept
{
    return target_;
}

/// @brief Returns the complete bounded header collection without exposing mutation.
std::span<const HttpHeaderView> HttpRequestView::fields() const noexcept
{
    return headers_;
}

/// @brief Returns exactly the body bytes declared by Content-Length.
std::span<const std::byte> HttpRequestView::body() const noexcept
{
    return body_;
}

/// @brief Finds the first case-insensitive header-name match.
std::string_view HttpRequestView::header(const std::string_view name) const noexcept
{
    const auto match =
        std::ranges::find_if(headers_, [name](const HttpHeaderView &field)
                             { return ascii_case_insensitive_equal(field.name, name); });
    return match == headers_.end() ? std::string_view{} : match->value;
}

/// @brief Collects every case-insensitive header-name match in source order.
std::vector<std::string_view> HttpRequestView::headers(const std::string_view name) const
{
    std::vector<std::string_view> values;
    values.reserve(headers_.size());
    std::ranges::for_each(headers_,
                          [&values, name](const HttpHeaderView &field)
                          {
                              if (ascii_case_insensitive_equal(field.name, name))
                              {
                                  values.push_back(field.value);
                              }
                          });
    return values;
}

/// @brief Converts a supported method into its canonical HTTP spelling.
std::string_view to_string(const HttpMethod method) noexcept
{
    switch (method)
    {
    case HttpMethod::get:
        return "GET";
    case HttpMethod::head:
        return "HEAD";
    case HttpMethod::post:
        return "POST";
    case HttpMethod::put:
        return "PUT";
    case HttpMethod::delete_method:
        return "DELETE";
    case HttpMethod::options:
        return "OPTIONS";
    }
    return "UNKNOWN";
}

} // namespace sparenode::http
