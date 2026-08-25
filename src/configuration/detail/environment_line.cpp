#include "sparenode/configuration/detail/environment_line.hpp"

#include <cctype>

namespace sparenode::configuration::detail
{
namespace
{

/// @brief Removes leading and trailing ASCII whitespace from a non-owning string.
/// @param[in] text Text whose surrounding whitespace should be ignored.
/// @return A view into the trimmed portion of text.
[[nodiscard]] std::string_view trim(const std::string_view text) noexcept
{
    const auto is_space = [](const char character)
    { return std::isspace(static_cast<unsigned char>(character)) != 0; };

    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin]))
    {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1]))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

ParsedEnvironmentLine parse_environment_line(const std::string_view line,
                                             const std::size_t line_number)
{
    auto content = trim(line);
    constexpr std::string_view utf8_byte_order_mark = "\xEF\xBB\xBF";
    if (line_number == 1 && content.starts_with(utf8_byte_order_mark))
    {
        content.remove_prefix(utf8_byte_order_mark.size());
        content = trim(content);
    }
    if (content.empty() || content.front() == '#')
    {
        return std::monostate{};
    }

    const auto separator = content.find('=');
    if (separator == std::string_view::npos)
    {
        return MalformedEnvironmentLine{};
    }

    const auto key = trim(content.substr(0, separator));
    auto value = trim(content.substr(separator + 1));
    if (key.empty())
    {
        return MalformedEnvironmentLine{};
    }

    if (!value.empty() && (value.front() == '\'' || value.front() == '"'))
    {
        if (value.size() < 2 || value.back() != value.front())
        {
            return MalformedEnvironmentLine{std::string(key)};
        }
        value = value.substr(1, value.size() - 2);
    }

    return EnvironmentAssignment{std::string(key), std::string(value)};
}

} // namespace sparenode::configuration::detail
