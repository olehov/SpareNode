#include "sparenode/configuration/environment_file.hpp"

#include <fstream>
#include <utility>
#include <variant>

#include "sparenode/configuration/detail/environment_line.hpp"

namespace sparenode::configuration
{
EnvironmentFile::EnvironmentFile(Variables variables) : variables_(std::move(variables))
{
}

Result<EnvironmentFile, EnvironmentFileError> EnvironmentFile::load_default()
{
    return load(std::filesystem::path(default_file_name));
}

Result<EnvironmentFile, EnvironmentFileError>
EnvironmentFile::load(const std::filesystem::path &source_path)
{
    std::ifstream input(source_path);
    if (!input.is_open())
    {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(source_path, filesystem_error);
        const auto error_code = !filesystem_error && !exists
                                    ? EnvironmentFileErrorCode::file_not_found
                                    : EnvironmentFileErrorCode::file_unreadable;
        return unexpected(EnvironmentFileError{error_code, source_path, 0, {}});
    }

    Variables variables;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line))
    {
        auto parsed_line = detail::parse_environment_line(line, ++line_number);
        if (std::holds_alternative<detail::MalformedEnvironmentLine>(parsed_line))
        {
            auto malformed_line =
                std::get<detail::MalformedEnvironmentLine>(std::move(parsed_line));
            return unexpected(EnvironmentFileError{EnvironmentFileErrorCode::malformed_entry,
                                                   source_path, line_number,
                                                   std::move(malformed_line.variable)});
        }
        if (!std::holds_alternative<detail::EnvironmentAssignment>(parsed_line))
        {
            continue;
        }

        auto assignment = std::get<detail::EnvironmentAssignment>(std::move(parsed_line));
        auto [position, inserted] =
            variables.emplace(std::move(assignment.key), std::move(assignment.value));
        if (!inserted)
        {
            return unexpected(EnvironmentFileError{EnvironmentFileErrorCode::duplicate_variable,
                                                   source_path, line_number, position->first});
        }
    }

    if (input.bad())
    {
        return unexpected(
            EnvironmentFileError{EnvironmentFileErrorCode::file_unreadable, source_path, 0, {}});
    }
    return EnvironmentFile(std::move(variables));
}

const std::string *EnvironmentFile::find(const std::string_view variable) const noexcept
{
    const auto position = variables_.find(variable);
    return position == variables_.end() ? nullptr : &position->second;
}

std::size_t EnvironmentFile::size() const noexcept
{
    return variables_.size();
}

const char *to_string(const EnvironmentFileErrorCode code) noexcept
{
    switch (code)
    {
    case EnvironmentFileErrorCode::file_not_found:
        return "the environment file does not exist";
    case EnvironmentFileErrorCode::file_unreadable:
        return "the environment file could not be read";
    case EnvironmentFileErrorCode::malformed_entry:
        return "the environment file contains a malformed entry";
    case EnvironmentFileErrorCode::duplicate_variable:
        return "the environment file contains a duplicate variable";
    }

    return "unknown environment-file error";
}

} // namespace sparenode::configuration
