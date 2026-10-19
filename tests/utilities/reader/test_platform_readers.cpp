#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/io/fd_handle.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/replay/replay.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Requires the native Windows types above.
#include <winioctl.h>
#endif

using namespace dftracer::utils;
namespace files = dftracer::utils::io::platform_io;
namespace reader = dftracer::utils::utilities::reader::internal;
namespace compress = dftracer::utils::utilities::fileio::compress;

namespace {
class TestDirectory {
   public:
    TestDirectory() {
        static std::atomic<unsigned int> sequence{0};
        path =
            std::filesystem::temp_directory_path() /
            ("dftu_readers_" + std::to_string(process_id()) + "_" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directory(path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}  // namespace

TEST_CASE("gzip member indexing and random reads use portable file I/O") {
    TestDirectory dir;
    const auto path =
        utf8_path(dir.path / std::filesystem::path(u8"trace_\u0131_\u4e2d.gz"));
    const auto index_path = utf8_path(dir.path / "index");
    std::string text;
    std::vector<std::size_t> starts;
    {
        io::FdHandle fd(
            files::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
        REQUIRE(fd.get() >= 0);
        compress::GzipMemberCompressor compressor;
        REQUIRE(compressor.valid());
        for (int member = 0; member < 3; ++member) {
            starts.push_back(text.size());
            std::string part;
            for (int row = 0; row < 100; ++row)
                part += "{\"id\":" + std::to_string(member * 100 + row) + "}\n";
            auto compressed =
                compressor.compress_member(part.data(), part.size());
            REQUIRE(compressed.has_value());
            REQUIRE(files::write(fd.get(), compressed->data(),
                                 compressed->size()) ==
                    static_cast<io::IoResult>(compressed->size()));
            text += part;
        }
    }
    auto indexer = std::make_shared<index::gzip::GzipIndexer>(path, index_path,
                                                              1u << 20, true);
    dftu_utils_test::run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        co_await indexer->build_async();
    });
    REQUIRE(indexer->get_max_bytes() == text.size());
    REQUIRE(indexer->get_members().size() == 3);
    reader::GzipReader input(indexer);
    for (const auto start : starts) {
        auto stream =
            input.stream(reader::StreamConfig()
                             .stream_type(reader::StreamType::BYTES)
                             .range_type(reader::RangeType::BYTE_RANGE)
                             .from(start)
                             .to(text.size()));
        std::string actual;
        dftu_utils_test::run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<char> buffer(137);
            while (const auto count = co_await stream->read_async(
                       buffer.data(), buffer.size()))
                actual.append(buffer.data(), count);
        });
        CHECK(actual == text.substr(start));
    }
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
TEST_CASE("Arrow IPC maps UTF-8 paths and keeps moved readers usable") {
    using namespace dftracer::utils::utilities::common::arrow;
    TestDirectory dir;
    const auto path = utf8_path(
        dir.path / std::filesystem::path(u8"batch_\u0131_\u4e2d.arrow"));
    dftu_utils_test::run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        RecordBatchBuilder builder;
        builder.declare_schema(
            {{"id", ColumnType::INT64}, {"name", ColumnType::STRING}});
        builder.reserve(2);
        for (int row = 0; row < 2; ++row) {
            builder.append_int64(0, row);
            builder.append_string(1, "value");
            builder.end_row();
        }
        auto batch = builder.finish();
        IpcWriter writer;
        const auto opened = co_await writer.open(path);
        REQUIRE(opened == 0);
        const auto written = co_await writer.write_batch(batch);
        REQUIRE(written == 0);
        const auto closed = co_await writer.close();
        REQUIRE(closed == 0);
    });
    IpcReader original;
    REQUIRE(original.open(path) == 0);
    IpcReader moved(std::move(original));
    CHECK_FALSE(original.is_open());
    REQUIRE(moved.num_batches() == 1);
    auto batch = moved.read_batch(0);
    REQUIRE(batch.valid());
    CHECK(batch.num_rows() == 2);
    CHECK(batch.num_columns() == 2);
    moved.close();

    const auto short_path = utf8_path(dir.path / "short.arrow");
    {
        io::FdHandle fd(
            files::open(short_path.c_str(), O_CREAT | O_RDWR, 0600));
        REQUIRE(fd.get() >= 0);
        REQUIRE(files::write(fd.get(), "ARROW1", 6) == 6);
    }
    CHECK(moved.open(short_path) != 0);
    CHECK_FALSE(moved.is_open());
    CHECK(moved.open(path) == 0);
}
#endif

TEST_CASE("replay preserves UTF-8 paths and 64-bit seek offsets") {
    using namespace dftracer::utils::utilities::replay;
    TestDirectory dir;
    const auto output = dir.path / std::filesystem::path(u8"replay_\u4e2d");
    ReplayConfig config;
    config.output_directory = utf8_path(output);
    PosixExecutor executor;
    Trace trace;
    trace.cat = "posix";
    trace.fhash = "large";
    trace.func_name = "open";
    REQUIRE(executor.execute(trace, config));
    const auto path = utf8_path(output / "replay_file_large");
    io::FdHandle fd(files::open(path.c_str(), O_RDWR));
    REQUIRE(fd.get() >= 0);
#ifdef _WIN32
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(fd.get())),
                            FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                            nullptr));
#endif
    trace.func_name = "lseek";
    trace.offset = (std::int64_t{1} << 32) + 19;
    REQUIRE(executor.execute(trace, config));
    trace.func_name = "write";
    trace.size = 1;
    REQUIRE(executor.execute(trace, config));
    trace.func_name = "close";
    REQUIRE(executor.execute(trace, config));
    io::FileStat info{};
    REQUIRE(files::stat_fd(fd.get(), &info) == 0);
    CHECK(info.st_size == trace.offset + 1);
    char value = 0;
    CHECK(files::pread(fd.get(), &value, 1, trace.offset) == 1);
    CHECK(value == 'A');
}
