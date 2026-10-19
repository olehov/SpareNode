#include "handlers.hpp"

#include <new>
#include <optional>
#include <utility>

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/filesystem/upload_finalizer.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/storage/temporary_file.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_upload(const FilesystemApiLocation &location,
                                                   const HttpRequestView &request,
                                                   const std::string_view requested_path)
{
    try
    {
        if (!location.permissions.allows_write())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"write_forbidden\"}");
        }

        auto retained_artifact = request.temporary_body();
        std::optional<storage::TemporaryFile> materialized_artifact;
        const storage::TemporaryFile *source = retained_artifact.get();
        if (source == nullptr)
        {
            if (request.body_size() != request.body().size())
            {
                return make_json_response(HttpStatusCode::bad_request,
                                          "{\"error\":\"invalid_upload\"}");
            }
            auto created = storage::TemporaryFile::create();
            if (!created)
            {
                return make_public_error_response(created.error());
            }
            materialized_artifact.emplace(std::move(created).value());
            const auto options = request.temporary_file_options();
            if (auto written = materialized_artifact->write(request.body(), options); !written)
            {
                return make_public_error_response(written.error());
            }
            if (auto completed = materialized_artifact->complete(options); !completed)
            {
                return make_public_error_response(completed.error());
            }
            source = &materialized_artifact.value();
        }

        auto published = filesystem::finalize_upload(location.root, requested_path, *source,
                                                     request.temporary_file_options());
        if (!published)
        {
            return make_public_error_response(published.error());
        }
        return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(filesystem::UploadFinalizationErrorCode::resource_failure)});
    }
}

} // namespace sparenode::http::detail
