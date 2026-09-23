#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "support/temporary_directory.hpp"
#ifdef _WIN32
#include "support/windows_junction.hpp"
#endif

namespace
{

/// @brief Owns a shared root and a real protected sibling for sandbox read attempts.
struct FilesystemSecurityFixture
{
    static constexpr std::string_view allowed_content = "allowed-content"; ///< In-root marker.
    static constexpr std::string_view protected_content =
        "protected-content"; ///< Out-of-root marker.

    sparenode::test::TemporaryDirectory directory{
        "sparenode-filesystem-security"};                      ///< Isolated test tree.
    std::filesystem::path shared{directory.path() / "shared"}; ///< Configured sandbox root.
    std::filesystem::path protected_directory{directory.path() /
                                              "protected"}; ///< Sibling outside the sandbox.
    std::filesystem::path protected_file{protected_directory /
                                         "secret.txt"}; ///< Real disclosure target.

    /// @brief Creates readable files on both sides of the security boundary.
    FilesystemSecurityFixture()
    {
        std::filesystem::create_directories(shared / "nested");
        std::filesystem::create_directory(protected_directory);
        REQUIRE(std::ofstream(shared / "public.txt", std::ios::binary) << allowed_content);
        REQUIRE(std::ofstream(shared / "nested" / "inside.txt", std::ios::binary)
                << allowed_content);
        REQUIRE(std::ofstream(protected_file, std::ios::binary) << protected_content);
    }

    /// @brief Resolves a request through the sandbox and reads the resulting regular file.
    /// @param[in] request Untrusted request path to resolve.
    /// @return File contents, or no value when resolution or opening is denied.
    [[nodiscard]] std::optional<std::string> read_request(const std::string_view request) const
    {
        auto root = sparenode::configuration::SharedRoot::create(shared);
        REQUIRE(root);
        auto resolved = sparenode::filesystem::SafePath::resolve(root.value(), request);
        if (!resolved)
        {
            return std::nullopt;
        }
        std::ifstream input(resolved->path(), std::ios::binary);
        if (!input)
        {
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
};

/// @brief Converts a native path to the byte-oriented UTF-8 request format.
/// @param[in] path Host path to convert.
/// @return UTF-8 bytes containing the same path.
[[nodiscard]] std::string path_as_utf8(const std::filesystem::path &path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

/// @brief Creates a real directory symbolic link or skips an unavailable local privilege.
/// @param[in] target Stored directory-link target.
/// @param[in] link New symbolic-link path.
void create_directory_link(const std::filesystem::path &target, const std::filesystem::path &link)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
#if defined(_WIN32) && !defined(SPARENODE_REQUIRE_SYMLINK_TESTS)
    constexpr int privilege_not_held = 1314; // Win32 ERROR_PRIVILEGE_NOT_HELD.
    if (error == std::error_code(privilege_not_held, std::system_category()))
    {
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
    }
#endif
    INFO(error.message());
    REQUIRE(error == std::error_code{});
}

} // namespace

TEST_CASE("Filesystem security suite reads valid paths only inside the shared root",
          "[filesystem][security][sandbox]")
{
    const FilesystemSecurityFixture fixture;
    constexpr std::array requests{"public.txt", "./public.txt", "nested/../public.txt",
                                  "nested\\inside.txt", "nested%2Finside.txt"};

    for (const auto request : requests)
    {
        CAPTURE(request);
        const auto content = fixture.read_request(request);
        REQUIRE(content == std::optional<std::string>{FilesystemSecurityFixture::allowed_content});
    }
}

TEST_CASE("Filesystem security suite cannot read a protected sibling through path attacks",
          "[filesystem][security][sandbox]")
{
    const FilesystemSecurityFixture fixture;
    auto requests = std::vector<std::string>{
        "../protected/secret.txt",
        "..\\protected\\secret.txt",
        "nested/../../protected/secret.txt",
        "%2e%2e%2fprotected%2fsecret.txt",
        "%2E%2E%5Cprotected%5Csecret.txt",
        "nested%5c..%5c..%5cprotected%5csecret.txt",
        "%252e%252e%252fprotected%252fsecret.txt",
        "%",
        "%2",
        "%GG",
    };
    requests.push_back(path_as_utf8(fixture.protected_file));
    auto embedded_null = std::string("../protected/secret.txt");
    embedded_null.push_back('\0');
    embedded_null += "ignored";
    requests.push_back(std::move(embedded_null));
    auto invalid_utf8 = std::string("../protected/secret.txt");
    invalid_utf8.push_back(static_cast<char>(0xC0));
    invalid_utf8.push_back(static_cast<char>(0xAF));
    requests.push_back(std::move(invalid_utf8));

    std::ifstream protected_input(fixture.protected_file, std::ios::binary);
    REQUIRE(protected_input);
    const auto protected_text = std::string(std::istreambuf_iterator<char>(protected_input),
                                            std::istreambuf_iterator<char>());
    REQUIRE(protected_text == FilesystemSecurityFixture::protected_content);
    for (const auto &request : requests)
    {
        CAPTURE(request);
        REQUIRE(fixture.read_request(request) == std::nullopt);
    }
}

TEST_CASE("Filesystem security suite blocks direct and nested symbolic-link reads",
          "[filesystem][security][sandbox][symlink]")
{
    const FilesystemSecurityFixture fixture;
    create_directory_link(fixture.protected_directory, fixture.shared / "external");
    create_directory_link("external", fixture.shared / "nested-external");

    REQUIRE(fixture.read_request("external/secret.txt") == std::nullopt);
    REQUIRE(fixture.read_request("nested-external/secret.txt") == std::nullopt);
}

#ifdef _WIN32
TEST_CASE("Filesystem security suite blocks direct and nested junction reads",
          "[filesystem][security][sandbox][windows][junction][reparse]")
{
    const FilesystemSecurityFixture fixture;
    const auto middle = fixture.shared / "middle";
    std::filesystem::create_directory(middle);

    auto error = sparenode::test::create_directory_junction(fixture.protected_directory,
                                                            fixture.shared / "external");
    INFO(error.message());
    REQUIRE(error == std::error_code{});
    error = sparenode::test::create_directory_junction(fixture.protected_directory,
                                                       middle / "external");
    INFO(error.message());
    REQUIRE(error == std::error_code{});
    error = sparenode::test::create_directory_junction(middle, fixture.shared / "nested-external");
    INFO(error.message());
    REQUIRE(error == std::error_code{});

    REQUIRE(fixture.read_request("external/secret.txt") == std::nullopt);
    REQUIRE(fixture.read_request("nested-external/external/secret.txt") == std::nullopt);
}
#endif
