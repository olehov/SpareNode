#include "sparenode/http/mime_type_registry.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace sparenode::http
{
namespace
{

constexpr std::string_view binary_fallback = "application/octet-stream";
constexpr std::string_view safe_text_type = "text/plain; charset=utf-8";

/// Browser-active extensions that configuration cannot opt into direct execution.
///
/// These files are always served as plain text, even when `mime.types` assigns
/// a different media type, so user configuration cannot weaken the inline
/// content policy for HTML, scripts, styles, SVG, or XML transformations.
constexpr std::array active_extensions{
    std::string_view{".css"},  std::string_view{".htm"}, std::string_view{".html"},
    std::string_view{".js"},   std::string_view{".mjs"}, std::string_view{".svg"},
    std::string_view{".svgz"}, std::string_view{".xht"}, std::string_view{".xhtml"},
    std::string_view{".xml"},  std::string_view{".xsl"}, std::string_view{".xslt"},
};

/// @brief Reports whether one ASCII byte is valid in an HTTP token.
/// @param[in] byte Byte to classify without locale-dependent rules.
/// @return `true` for an ASCII alphanumeric or permitted token punctuation byte.
[[nodiscard]] bool is_token_character(const unsigned char byte) noexcept
{
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    const bool alpha_numeric = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                               (byte >= 'a' && byte <= 'z');
    return alpha_numeric || punctuation.contains(static_cast<char>(byte));
}

/// @brief Converts one ASCII uppercase byte to lowercase.
/// @param[in] byte Byte to normalize without locale-dependent rules.
/// @return Lowercase ASCII byte, or the original byte when no conversion is needed.
[[nodiscard]] constexpr char ascii_lower(const unsigned char byte) noexcept
{
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A'))
                                      : static_cast<char>(byte);
}

/// @brief Validates one bounded parameter-free `type/subtype` value.
/// @param[in] value Media type to validate.
/// @return `true` when both sides of its single slash are valid HTTP tokens.
[[nodiscard]] bool is_valid_media_type(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > MimeTypeRegistry::maximum_media_type_bytes)
    {
        return false;
    }
    const auto separator = value.find('/');
    if (separator == 0 || separator == std::string_view::npos || separator + 1 == value.size() ||
        value.find('/', separator + 1) != std::string_view::npos)
    {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char byte)
                               { return byte == '/' || is_token_character(byte); });
}

/// @brief Normalizes one configured extension to lowercase with a leading dot.
/// @param[in,out] extension Extension to validate and normalize in place.
/// @return `true` when the extension satisfies the registry policy.
[[nodiscard]] bool normalize_extension(std::string &extension)
{
    if (!extension.empty() && extension.front() == '.')
    {
        extension.erase(extension.begin());
    }
    if (extension.empty() || extension.size() > MimeTypeRegistry::maximum_extension_bytes ||
        !std::ranges::all_of(extension,
                             [](const unsigned char byte)
                             {
                                 const bool alpha_numeric = (byte >= '0' && byte <= '9') ||
                                                            (byte >= 'A' && byte <= 'Z') ||
                                                            (byte >= 'a' && byte <= 'z');
                                 return alpha_numeric || byte == '+' || byte == '-' || byte == '_';
                             }))
    {
        return false;
    }
    std::ranges::transform(extension, extension.begin(),
                           [](const unsigned char byte) { return ascii_lower(byte); });
    extension.insert(extension.begin(), '.');
    return true;
}

/// @brief Reports whether an extension is covered by the fixed active-content policy.
/// @param[in] extension Normalized lowercase extension including its leading dot.
/// @return `true` when the file must be served as plain text.
[[nodiscard]] bool is_active_extension(const std::string_view extension) noexcept
{
    return std::ranges::binary_search(active_extensions, extension);
}

/// @brief Reports whether a media type could activate browser-controlled content.
/// @param[in] media_type Validated lowercase parameter-free media type.
/// @return `true` when the configured type must be replaced with safe plain text.
[[nodiscard]] bool is_active_media_type(const std::string_view media_type) noexcept
{
    if (media_type == "application/wasm" || media_type == "text/css" || media_type == "text/html" ||
        media_type.ends_with("+xml"))
    {
        return true;
    }
    const auto separator = media_type.find('/');
    const auto subtype = media_type.substr(separator + 1);
    return subtype == "xml" || subtype.contains("javascript") || subtype.contains("ecmascript") ||
           media_type == "text/jscript" || media_type == "text/livescript";
}

} // namespace

Result<MimeTypeRegistry, MimeTypeRegistryError>
MimeTypeRegistry::create(std::vector<MimeTypeMapping> mappings)
{
    try
    {
        if (mappings.size() > maximum_entries)
        {
            return unexpected(MimeTypeRegistryError{MimeTypeRegistryErrorCode::too_many_entries,
                                                    maximum_entries});
        }
        std::unordered_set<std::string> extensions;
        extensions.reserve(mappings.size());
        for (std::size_t index = 0; index < mappings.size(); ++index)
        {
            auto &mapping = mappings[index];
            if (!normalize_extension(mapping.extension))
            {
                return unexpected(
                    MimeTypeRegistryError{MimeTypeRegistryErrorCode::invalid_extension, index});
            }
            if (!is_valid_media_type(mapping.media_type))
            {
                return unexpected(
                    MimeTypeRegistryError{MimeTypeRegistryErrorCode::invalid_media_type, index});
            }
            std::ranges::transform(mapping.media_type, mapping.media_type.begin(),
                                   [](const unsigned char byte) { return ascii_lower(byte); });
            if (!extensions.insert(mapping.extension).second)
            {
                return unexpected(
                    MimeTypeRegistryError{MimeTypeRegistryErrorCode::duplicate_extension, index});
            }
        }
        std::ranges::stable_sort(mappings, {}, &MimeTypeMapping::extension);
        return MimeTypeRegistry(std::move(mappings));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            MimeTypeRegistryError{MimeTypeRegistryErrorCode::resource_allocation_failed, 0});
    }
}

std::string_view MimeTypeRegistry::content_type_for_path(const std::string_view path) const noexcept
{
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ||
        dot + 1 == path.size())
    {
        return binary_fallback;
    }

    const auto extension_length = path.size() - dot;
    if (extension_length > maximum_extension_bytes + 1)
    {
        return binary_fallback;
    }
    std::array<char, maximum_extension_bytes + 1> normalized{};
    std::ranges::transform(path.substr(dot), normalized.begin(),
                           [](const unsigned char byte) { return ascii_lower(byte); });
    const std::string_view extension(normalized.data(), extension_length);
    if (is_active_extension(extension))
    {
        return safe_text_type;
    }
    const auto mapping =
        std::ranges::lower_bound(mappings_, extension, {}, &MimeTypeMapping::extension);
    if (mapping == mappings_.end() || mapping->extension != extension)
    {
        return binary_fallback;
    }
    return is_active_media_type(mapping->media_type) ? safe_text_type
                                                     : std::string_view(mapping->media_type);
}

const char *to_string(const MimeTypeRegistryErrorCode code) noexcept
{
    switch (code)
    {
    case MimeTypeRegistryErrorCode::invalid_extension:
        return "invalid MIME filename extension";
    case MimeTypeRegistryErrorCode::invalid_media_type:
        return "invalid MIME media type";
    case MimeTypeRegistryErrorCode::duplicate_extension:
        return "duplicate MIME filename extension";
    case MimeTypeRegistryErrorCode::too_many_entries:
        return "too many MIME type mappings";
    case MimeTypeRegistryErrorCode::resource_allocation_failed:
        return "MIME registry memory allocation failed";
    }
    return "unknown MIME registry error";
}

} // namespace sparenode::http
