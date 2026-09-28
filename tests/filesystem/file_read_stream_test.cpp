#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stop_token>
#include <string>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "support/temporary_directory.hpp"

namespace
{

[[nodiscard]] sparenode::configuration::SharedRoot
make_root(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    return std::move(root).value();
}

void write_file(const std::filesystem::path &path, const std::string_view content)
{
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

[[nodiscard]] std::string read_all(sparenode::filesystem::FileReadStream &stream,
                                   const std::size_t chunk_size)
{
    std::array<std::byte, 16> buffer{};
    REQUIRE(chunk_size <= buffer.size());
    std::string output;
    while (true)
    {
        auto chunk = stream.read(std::span(buffer).first(chunk_size), {});
        REQUIRE(chunk);
        REQUIRE(chunk.value() <= chunk_size);
        if (chunk.value() == 0)
        {
            break;
        }
        output.append(reinterpret_cast<const char *>(buffer.data()), chunk.value());
    }
    return output;
}

} // namespace

TEST_CASE("File read stream incrementally reads a confined regular file",
          "[filesystem][file-read][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-file-read");
    constexpr std::string_view content = "bounded file payload";
    write_file(directory.path() / "payload.bin", content);

    auto stream = sparenode::filesystem::FileReadStream::open(make_root(directory), "payload.bin");
    REQUIRE(stream);
    CHECK(stream->size() == content.size());
    CHECK(read_all(stream.value(), 3) == content);
}

TEST_CASE("File read cancellation does not advance the stream",
          "[filesystem][file-read][cancellation]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-file-cancel");
    write_file(directory.path() / "payload.bin", "abcdef");
    auto stream = sparenode::filesystem::FileReadStream::open(make_root(directory), "payload.bin");
    REQUIRE(stream);

    std::stop_source source;
    source.request_stop();
    std::array<std::byte, 2> buffer{};
    const auto cancelled = stream->read(buffer, source.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == sparenode::filesystem::FileReadErrorCode::cancelled);
    CHECK(read_all(stream.value(), buffer.size()) == "abcdef");
}

TEST_CASE("File read stream rejects missing, directory, and escaping paths",
          "[filesystem][file-read][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-file-errors");
    std::filesystem::create_directory(directory.path() / "folder");
    const auto root = make_root(directory);

    const auto missing = sparenode::filesystem::FileReadStream::open(root, "missing.bin");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == sparenode::filesystem::FileReadErrorCode::not_found);

    const auto folder = sparenode::filesystem::FileReadStream::open(root, "folder");
    REQUIRE_FALSE(folder);
    CHECK(folder.error().code == sparenode::filesystem::FileReadErrorCode::not_regular_file);

    const auto traversal = sparenode::filesystem::FileReadStream::open(root, "../outside.bin");
    REQUIRE_FALSE(traversal);
    CHECK(traversal.error().code == sparenode::filesystem::FileReadErrorCode::invalid_path);
}

TEST_CASE("File read stream stays bound to the opened file after path replacement",
          "[filesystem][file-read][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-file-stable-handle");
    const auto requested = directory.path() / "payload.bin";
    const auto renamed = directory.path() / "original.bin";
    write_file(requested, "original");
    auto stream = sparenode::filesystem::FileReadStream::open(make_root(directory), "payload.bin");
    REQUIRE(stream);

    std::filesystem::rename(requested, renamed);
    write_file(requested, "replacement");

    CHECK(read_all(stream.value(), 2) == "original");
}
