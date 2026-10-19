#include "json_writer.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace sparenode::http::detail
{
namespace
{

/// @brief Formats one second-resolution system time as RFC 3339 UTC.
[[nodiscard]] std::string format_timestamp(const std::chrono::sys_seconds value)
{
    const auto day_point = std::chrono::floor<std::chrono::days>(value);
    const std::chrono::year_month_day date(day_point);
    const std::chrono::hh_mm_ss time(value - day_point);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
           << std::setw(2) << static_cast<unsigned int>(date.month()) << '-' << std::setw(2)
           << static_cast<unsigned int>(date.day()) << 'T' << std::setw(2) << time.hours().count()
           << ':' << std::setw(2) << time.minutes().count() << ':' << std::setw(2)
           << time.seconds().count() << 'Z';
    return output.str();
}

/// @brief Returns the JSON label for one portable entry category.
[[nodiscard]] constexpr std::string_view
entry_type_name(const filesystem::DirectoryEntryType type) noexcept
{
    switch (type)
    {
    case filesystem::DirectoryEntryType::regular_file:
        return "file";
    case filesystem::DirectoryEntryType::directory:
        return "directory";
    case filesystem::DirectoryEntryType::symbolic_link:
        return "symlink";
    case filesystem::DirectoryEntryType::other:
        return "other";
    }
    return "other";
}

} // namespace

void append_json_string(std::string &output, const std::string_view value)
{
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    output.push_back('"');
    for (const unsigned char byte : value)
    {
        switch (byte)
        {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            // JSON requires every control byte from 0x00 through 0x1F to be escaped.
            if (byte < 0x20U)
            {
                output.append("\\u00");
                // Convert the byte's high four bits into the first hexadecimal digit.
                output.push_back(hexadecimal[byte >> 4U]);
                // Mask the low four bits and convert them into the second hexadecimal digit.
                output.push_back(hexadecimal[byte & 0x0FU]);
            }
            else
            {
                output.push_back(static_cast<char>(byte));
            }
        }
    }
    output.push_back('"');
}

std::string serialize_listing(const std::vector<filesystem::DirectoryListingEntry> &entries)
{
    constexpr std::size_t outer_object_estimate = 16;
    constexpr std::size_t typical_entry_estimate = 96;
    std::string output;
    // Reserve a small outer JSON object plus typical serialized metadata for each entry.
    output.reserve(outer_object_estimate + entries.size() * typical_entry_estimate);
    output.append("{\"entries\":[");
    bool first = true;
    for (const auto &entry : entries)
    {
        if (!first)
        {
            output.push_back(',');
        }
        first = false;
        output.append("{\"name\":");
        append_json_string(output, entry.name);
        output.append(",\"type\":");
        append_json_string(output, entry_type_name(entry.type));
        output.append(",\"size\":");
        output.append(entry.size ? std::to_string(entry.size.value()) : "null");
        output.append(",\"modifiedAt\":");
        append_json_string(output, format_timestamp(entry.modification_time));
        output.push_back('}');
    }
    output.append("]}");
    return output;
}

} // namespace sparenode::http::detail
