#include "handlers.hpp"

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

#include "filesystem_error_response.hpp"
#include "sparenode/filesystem/detail/path_request_decoder.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::http::detail
{
namespace
{

/// @brief Converts a confined file stream into a fixed-length HTTP body source.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_file_response(filesystem::FileReadStream stream, const std::string_view content_type)
{
    const auto content_length = stream.size();
    HttpBodyReader reader = [stream = std::move(stream)](const std::span<std::byte> destination,
                                                         const std::stop_token &stop_token) mutable
        -> Result<std::size_t, HttpBodyReadError>
    {
        auto chunk = stream.read(destination, stop_token);
        if (chunk)
        {
            return chunk.value();
        }
        const auto &error = chunk.error();
        const auto domain = error.code == filesystem::FileReadErrorCode::cancelled
                                ? HttpBodyReadErrorDomain::cancellation
                                : HttpBodyReadErrorDomain::filesystem;
        return unexpected(HttpBodyReadError{domain, static_cast<int>(error.code)});
    };
    auto response = HttpResponse::create_streaming(HttpStatusCode::ok, "OK",
                                                   {{"Content-Type", std::string(content_type)},
                                                    {"Content-Disposition", "inline"},
                                                    {"X-Content-Type-Options", "nosniff"}},
                                                   content_length, std::move(reader));
    if (!response)
    {
        return unexpected(HttpRouteError{HttpRouteErrorCode::response_validation_failure,
                                         static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

} // namespace

Result<HttpResponse, HttpRouteError> handle_file(const FilesystemApiLocation &location,
                                                 const MimeTypeRegistry &mime_types,
                                                 const std::string_view requested_path)
{
    auto file = filesystem::FileReadStream::open(location.root, requested_path);
    if (!file)
    {
        return make_public_error_response(file.error());
    }
    const auto decoded_path = filesystem::detail::decode_path_request(requested_path);
    const auto content_type = decoded_path ? mime_types.content_type_for_path(decoded_path.value())
                                           : mime_types.content_type_for_path({});
    return make_file_response(std::move(file).value(), content_type);
}

} // namespace sparenode::http::detail
