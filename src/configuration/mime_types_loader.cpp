#include "sparenode/configuration/mime_types_loader.hpp"

#include <fstream>
#include <ios>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sparenode::configuration
{
namespace
{

[[nodiscard]] bool is_horizontal_space(const char value) noexcept
{
    return value == ' ' || value == '\t';
}

[[nodiscard]] std::vector<std::string_view> split_tokens(std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position < line.size())
    {
        while (position < line.size() && is_horizontal_space(line[position]))
        {
            ++position;
        }
        if (position == line.size() || line[position] == '#')
        {
            break;
        }
        const auto begin = position;
        while (position < line.size() && !is_horizontal_space(line[position]) &&
               line[position] != '#')
        {
            ++position;
        }
        tokens.push_back(line.substr(begin, position - begin));
        if (position < line.size() && line[position] == '#')
        {
            break;
        }
    }
    return tokens;
}

[[nodiscard]] MimeTypesLoadErrorCode
load_error_code(const http::MimeTypeRegistryErrorCode code) noexcept
{
    switch (code)
    {
    case http::MimeTypeRegistryErrorCode::invalid_extension:
        return MimeTypesLoadErrorCode::invalid_extension;
    case http::MimeTypeRegistryErrorCode::invalid_media_type:
        return MimeTypesLoadErrorCode::invalid_media_type;
    case http::MimeTypeRegistryErrorCode::duplicate_extension:
        return MimeTypesLoadErrorCode::duplicate_extension;
    case http::MimeTypeRegistryErrorCode::too_many_entries:
        return MimeTypesLoadErrorCode::too_many_entries;
    case http::MimeTypeRegistryErrorCode::resource_allocation_failed:
        return MimeTypesLoadErrorCode::resource_allocation_failed;
    }
    return MimeTypesLoadErrorCode::invalid_line;
}

/// @brief Appends all mappings from one bounded physical source line.
[[nodiscard]] std::optional<MimeTypesLoadError>
append_line_mappings(const std::string_view line, const std::size_t line_number,
                     std::vector<http::MimeTypeMapping> &mappings,
                     std::vector<std::size_t> &source_lines)
{
    if (line.size() > MimeTypesLoader::maximum_line_bytes)
    {
        return MimeTypesLoadError{MimeTypesLoadErrorCode::line_too_long, line_number};
    }
    const auto tokens = split_tokens(line);
    if (tokens.empty())
    {
        return std::nullopt;
    }
    if (tokens.size() < 2)
    {
        return MimeTypesLoadError{MimeTypesLoadErrorCode::invalid_line, line_number};
    }
    for (const auto extension : std::span(tokens).subspan(1))
    {
        if (mappings.size() == http::MimeTypeRegistry::maximum_entries)
        {
            return MimeTypesLoadError{MimeTypesLoadErrorCode::too_many_entries, line_number};
        }
        mappings.push_back({std::string(extension), std::string(tokens.front())});
        source_lines.push_back(line_number);
    }
    return std::nullopt;
}

} // namespace

Result<http::MimeTypeRegistry, MimeTypesLoadError>
MimeTypesLoader::load(const std::filesystem::path &source_path)
{
    try
    {
        std::ifstream input(source_path, std::ios::binary);
        if (!input.is_open())
        {
            return unexpected(MimeTypesLoadError{MimeTypesLoadErrorCode::open_failed, 1});
        }
        std::string source(maximum_file_bytes + 1, '\0');
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
        const auto bytes_read = input.gcount();
        if (input.bad())
        {
            return unexpected(MimeTypesLoadError{MimeTypesLoadErrorCode::read_failed, 1});
        }
        source.resize(static_cast<std::size_t>(bytes_read));
        if (source.size() > maximum_file_bytes)
        {
            return unexpected(MimeTypesLoadError{MimeTypesLoadErrorCode::file_too_large, 1});
        }
        return parse(source);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            MimeTypesLoadError{MimeTypesLoadErrorCode::resource_allocation_failed, 1});
    }
}

Result<http::MimeTypeRegistry, MimeTypesLoadError>
MimeTypesLoader::parse(const std::string_view source)
{
    try
    {
        if (source.size() > maximum_file_bytes)
        {
            return unexpected(MimeTypesLoadError{MimeTypesLoadErrorCode::file_too_large, 1});
        }
        std::vector<http::MimeTypeMapping> mappings;
        std::vector<std::size_t> source_lines;
        std::size_t line_number = 1;
        std::size_t line_begin = 0;
        while (line_begin <= source.size())
        {
            const auto line_end = source.find('\n', line_begin);
            const auto end = line_end == std::string_view::npos ? source.size() : line_end;
            auto line = source.substr(line_begin, end - line_begin);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (const auto failure =
                    append_line_mappings(line, line_number, mappings, source_lines))
            {
                return unexpected(*failure);
            }
            if (line_end == std::string_view::npos)
            {
                break;
            }
            line_begin = line_end + 1;
            ++line_number;
        }

        auto registry = http::MimeTypeRegistry::create(std::move(mappings));
        if (!registry)
        {
            const auto &failure = registry.error();
            const auto source_line = failure.mapping_index < source_lines.size()
                                         ? source_lines[failure.mapping_index]
                                         : line_number;
            return unexpected(MimeTypesLoadError{load_error_code(failure.code), source_line});
        }
        return std::move(registry).value();
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            MimeTypesLoadError{MimeTypesLoadErrorCode::resource_allocation_failed, 1});
    }
}

const char *to_string(const MimeTypesLoadErrorCode code) noexcept
{
    switch (code)
    {
    case MimeTypesLoadErrorCode::open_failed:
        return "unable to open MIME types file";
    case MimeTypesLoadErrorCode::read_failed:
        return "unable to read MIME types file";
    case MimeTypesLoadErrorCode::file_too_large:
        return "MIME types file exceeds the size limit";
    case MimeTypesLoadErrorCode::line_too_long:
        return "MIME types line exceeds the length limit";
    case MimeTypesLoadErrorCode::invalid_line:
        return "MIME types line requires a media type and at least one extension";
    case MimeTypesLoadErrorCode::invalid_extension:
        return "invalid MIME filename extension";
    case MimeTypesLoadErrorCode::invalid_media_type:
        return "invalid MIME media type";
    case MimeTypesLoadErrorCode::duplicate_extension:
        return "duplicate MIME filename extension";
    case MimeTypesLoadErrorCode::too_many_entries:
        return "too many MIME type mappings";
    case MimeTypesLoadErrorCode::resource_allocation_failed:
        return "MIME types memory allocation failed";
    }
    return "unknown MIME types loading error";
}

} // namespace sparenode::configuration
