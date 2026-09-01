#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sparenode::http
{

namespace detail
{
struct HttpRequestViewAccess;
}

/// @brief Identifies the HTTP methods accepted by the SpareNode v0.1 transport.
enum class HttpMethod : std::uint8_t
{
    get,           ///< Retrieve a resource.
    head,          ///< Retrieve response metadata without a body.
    post,          ///< Submit a bounded request body.
    put,           ///< Replace or upload a resource.
    delete_method, ///< Delete a resource.
    options        ///< Query endpoint capabilities.
};

/// @brief Borrows one parsed HTTP header from the caller-owned request buffer.
struct HttpHeaderView
{
    std::string_view name;  ///< Original case-preserving field name.
    std::string_view value; ///< Value with surrounding optional whitespace removed.
};

/// @brief Borrows a complete parsed HTTP/1.1 request without copying its bytes.
///
/// Every view remains valid only while the original input buffer remains alive,
/// unmoved, and unmodified. `body` contains exactly the declared Content-Length.
class HttpRequestView
{
  public:
    /// @brief Returns the validated supported HTTP method.
    /// @return Parsed request method.
    [[nodiscard]] HttpMethod method() const noexcept;

    /// @brief Returns the validated origin-form request target.
    /// @return Borrowed target beginning with `/`.
    [[nodiscard]] std::string_view target() const noexcept;

    /// @brief Returns all parsed header fields in their original source order.
    /// @return Read-only view over the bounded header collection.
    [[nodiscard]] std::span<const HttpHeaderView> fields() const noexcept;

    /// @brief Returns exactly the bytes declared by Content-Length.
    /// @return Borrowed read-only request body.
    [[nodiscard]] std::span<const std::byte> body() const noexcept;

    /// @brief Finds the first header using an ASCII case-insensitive name comparison.
    /// @param[in] name Header field name to locate.
    /// @return Borrowed header value, or an empty view when the field is absent.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

    /// @brief Finds every header using an ASCII case-insensitive name comparison.
    /// @param[in] name Header field name to locate.
    /// @return Borrowed values in their original source order.
    [[nodiscard]] std::vector<std::string_view> headers(std::string_view name) const;

  private:
    friend struct detail::HttpRequestViewAccess;

    /// @brief Creates a complete view after every parser invariant has passed.
    /// @param[in] method Validated supported method.
    /// @param[in] target Validated origin-form target.
    /// @param[in] headers Bounded validated header fields in source order.
    /// @param[in] body Exact borrowed body boundary.
    HttpRequestView(HttpMethod method, std::string_view target, std::vector<HttpHeaderView> headers,
                    std::span<const std::byte> body);

    HttpMethod method_{};                 ///< Parsed supported request method.
    std::string_view target_;             ///< Origin-form request target beginning with `/`.
    std::vector<HttpHeaderView> headers_; ///< Bounded headers in source order.
    std::span<const std::byte> body_;     ///< Exact request body boundary.
};

/// @brief Returns the canonical uppercase spelling of a supported HTTP method.
/// @param[in] method Supported method to describe.
/// @return Static method text suitable for protocol diagnostics.
[[nodiscard]] std::string_view to_string(HttpMethod method) noexcept;

} // namespace sparenode::http
