#include "sparenode/filesystem/directory_listing.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <new>
#include <ranges>
#include <string>
#include <utility>

namespace sparenode::filesystem
{
namespace
{

/// @brief Maps a host error into a stable listing category.
[[nodiscard]] DirectoryListingError filesystem_error(const std::error_code error) noexcept
{
    if (error == std::errc::no_such_file_or_directory)
    {
        return {DirectoryListingErrorCode::not_found, error, std::nullopt};
    }
    if (error == std::errc::permission_denied)
    {
        return {DirectoryListingErrorCode::permission_denied, error, std::nullopt};
    }
    return {DirectoryListingErrorCode::filesystem_failure, error, std::nullopt};
}

/// @brief Percent-encodes one UTF-8 basename for a second SafePath validation pass.
[[nodiscard]] std::string encode_path_component(const std::string_view component)
{
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    
    // One input byte can expand to the three-character percent escape `%HH`.
    std::string encoded;
    encoded.reserve(component.size() * 3);

    for (const unsigned char byte : component)
    {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        if (unreserved)
        {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hexadecimal[byte >> 4U]);
        encoded.push_back(hexadecimal[byte & 0x0FU]);
    }
    return encoded;
}

/// @brief Converts a native basename into the public UTF-8 representation.
[[nodiscard]] std::string filename_as_utf8(const std::filesystem::path &path)
{
    const auto value = path.filename().u8string();
    return {value.begin(), value.end()};
}

/// @brief Groups the two distinct values required to build one child request.
struct ChildRequestParts
{
    std::string_view parent;       ///< Original encoded parent request.
    std::string_view encoded_name; ///< Encoded native child basename.
};

/// @brief Joins the original directory request with an encoded child component.
[[nodiscard]] std::string child_request(const ChildRequestParts &parts)
{
    std::string request(parts.parent);
    if (!request.empty() && request.back() != '/' && request.back() != '\\')
    {
        request.push_back('/');
    }
    request.append(parts.encoded_name);
    return request;
}

/// @brief Keeps link-object and resolved-target status roles explicit at call sites.
struct EntryStatuses
{
    std::filesystem::file_status link;   ///< Status without following the original link.
    std::filesystem::file_status target; ///< Status of the safely resolved target.
};

/// @brief Classifies an entry without following its original symbolic-link object.
[[nodiscard]] DirectoryEntryType classify_entry(const EntryStatuses statuses)
{
    if (std::filesystem::is_symlink(statuses.link))
    {
        return DirectoryEntryType::symbolic_link;
    }
    if (std::filesystem::is_regular_file(statuses.target))
    {
        return DirectoryEntryType::regular_file;
    }
    if (std::filesystem::is_directory(statuses.target))
    {
        return DirectoryEntryType::directory;
    }
    return DirectoryEntryType::other;
}

/// @brief Converts an implementation-defined filesystem clock into system time.
[[nodiscard]] std::chrono::sys_seconds
to_system_seconds(const std::filesystem::file_time_type value) noexcept
{
    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto system_now = std::chrono::system_clock::now();
    const auto system_value =
        system_now +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(value - file_now);
    return std::chrono::floor<std::chrono::seconds>(system_value);
}

/// @brief Resolves and verifies the requested listing directory.
[[nodiscard]] Result<SafePath, DirectoryListingError>
resolve_directory(const configuration::SharedRoot &shared_root,
                  const std::string_view requested_path)
{
    auto directory = SafePath::resolve(shared_root, requested_path);
    if (!directory)
    {
        return unexpected(DirectoryListingError{
            DirectoryListingErrorCode::invalid_path, {}, directory.error().code});
    }
    std::error_code error;
    const auto status = std::filesystem::status(directory->path(), error);
    if (error)
    {
        return unexpected(filesystem_error(error));
    }
    if (!std::filesystem::exists(status))
    {
        return unexpected(
            DirectoryListingError{DirectoryListingErrorCode::not_found, {}, std::nullopt});
    }
    if (!std::filesystem::is_directory(status))
    {
        return unexpected(
            DirectoryListingError{DirectoryListingErrorCode::not_directory, {}, std::nullopt});
    }
    return std::move(directory).value();
}

/// @brief Reads one entry after independently confining its resolved target.
[[nodiscard]] Result<std::optional<DirectoryListingEntry>, DirectoryListingError>
read_entry(const configuration::SharedRoot &shared_root, const std::string_view requested_path,
           const std::filesystem::directory_entry &native_entry)
{
    const auto name = filename_as_utf8(native_entry.path());
    const auto encoded_name = encode_path_component(name);
    auto child = SafePath::resolve(shared_root, child_request({requested_path, encoded_name}));
    if (!child)
    {
        // External links and names that cannot form a safe later request expose no metadata.
        return std::optional<DirectoryListingEntry>{};
    }

    std::error_code error;
    const auto link_status = native_entry.symlink_status(error);
    if (error)
    {
        return unexpected(filesystem_error(error));
    }
    const auto target_status = std::filesystem::status(child->path(), error);
    if (error)
    {
        return unexpected(filesystem_error(error));
    }
    const auto type = classify_entry({link_status, target_status});

    std::optional<std::uintmax_t> size;
    if (type == DirectoryEntryType::regular_file)
    {
        size = std::filesystem::file_size(child->path(), error);
        if (error)
        {
            return unexpected(filesystem_error(error));
        }
    }
    const auto native_time = std::filesystem::last_write_time(child->path(), error);
    if (error)
    {
        return unexpected(filesystem_error(error));
    }
    return std::optional<DirectoryListingEntry>(std::in_place, name, type, size,
                                                to_system_seconds(native_time));
}

/// @brief Iterates one verified directory under the configured resource boundary.
[[nodiscard]] Result<std::vector<DirectoryListingEntry>, DirectoryListingError>
collect_entries(const configuration::SharedRoot &shared_root, const std::string_view requested_path,
                const SafePath &directory)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory.path(), error);
    if (error)
    {
        return unexpected(filesystem_error(error));
    }

    std::vector<DirectoryListingEntry> entries;
    std::size_t inspected_entries = 0;
    for (const auto end = std::filesystem::directory_iterator{}; iterator != end;
         iterator.increment(error))
    {
        if (error)
        {
            return unexpected(filesystem_error(error));
        }
        if (++inspected_entries > maximum_directory_listing_entries)
        {
            return unexpected(DirectoryListingError{
                DirectoryListingErrorCode::too_many_entries, {}, std::nullopt});
        }
        auto entry = read_entry(shared_root, requested_path, *iterator);
        if (!entry)
        {
            return unexpected(entry.error());
        }
        auto safe_entry = std::move(entry).value();
        if (safe_entry)
        {
            entries.push_back(std::move(*safe_entry));
        }
    }
    if (error)
    {
        return unexpected(filesystem_error(error));
    }
    std::ranges::sort(entries, {}, &DirectoryListingEntry::name);
    return entries;
}

} // namespace

Result<std::vector<DirectoryListingEntry>, DirectoryListingError>
list_directory(const configuration::SharedRoot &shared_root, const std::string_view requested_path)
{
    try
    {
        auto directory = resolve_directory(shared_root, requested_path);
        if (!directory)
        {
            return unexpected(directory.error());
        }
        return collect_entries(shared_root, requested_path, directory.value());
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(DirectoryListingError{
            DirectoryListingErrorCode::resource_allocation_failed, {}, std::nullopt});
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        return unexpected(filesystem_error(error.code()));
    }
    catch (const std::system_error &error)
    {
        return unexpected(filesystem_error(error.code()));
    }
}

const char *to_string(const DirectoryListingErrorCode code) noexcept
{
    switch (code)
    {
    case DirectoryListingErrorCode::invalid_path:
        return "directory path is invalid or outside the shared root";
    case DirectoryListingErrorCode::not_found:
        return "directory does not exist";
    case DirectoryListingErrorCode::not_directory:
        return "requested path is not a directory";
    case DirectoryListingErrorCode::permission_denied:
        return "directory metadata access was denied";
    case DirectoryListingErrorCode::too_many_entries:
        return "directory contains too many entries";
    case DirectoryListingErrorCode::filesystem_failure:
        return "directory listing failed";
    case DirectoryListingErrorCode::resource_allocation_failed:
        return "directory listing memory allocation failed";
    }
    return "unknown directory listing error";
}

} // namespace sparenode::filesystem
