#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/io/awaitable.h>
#include <dftracer/utils/core/io/file_ops.h>
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

namespace aio = dftracer::utils::io;
namespace files = dftracer::utils::io::file_ops;
namespace fs = std::filesystem;

namespace {

std::string utf8_path(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

class TemporaryDirectory {
   public:
    TemporaryDirectory() {
        static std::atomic<unsigned int> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("dftu_file_ops_" + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)));
        if (!fs::create_directory(path))
            throw std::runtime_error(
                "cannot create a unique file-I/O test directory");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

class TestFd {
   public:
    explicit TestFd(int fd) : value(fd) {}
    ~TestFd() {
        if (value >= 0) files::close(value);
    }
    TestFd(const TestFd&) = delete;
    TestFd& operator=(const TestFd&) = delete;
    int value;
};

}  // namespace

TEST_CASE("ready I/O results retain 64-bit values and negative errors") {
    const aio::IoResult value = (aio::IoResult{1} << 40) + 7;
    const auto ready = aio::IoAwaitable::ready(value);
    CHECK(ready.await_ready());
    CHECK(ready.await_resume() == value);
    CHECK(aio::IoAwaitable::ready(-EBADF).await_resume() == -EBADF);
}

TEST_CASE("binary sequential and positional I/O preserve bytes and position") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "binary.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    const std::array<char, 8> data{'a',  '\r', '\n', '\x1a',
                                   '\0', 'b',  'c',  'd'};
    REQUIRE(files::write(fd.value, data.data(), data.size()) == data.size());
    REQUIRE(files::seek(fd.value, 2, SEEK_SET) == 2);
    std::array<char, 8> read{};
    CHECK(files::pread(fd.value, read.data(), read.size(), 0) == data.size());
    CHECK(read == data);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 2);
    CHECK(files::pwrite(fd.value, "z", 1, 7) == 1);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 2);
    CHECK(files::pread(fd.value, read.data(), read.size(), 7) == 1);
    CHECK(read[0] == 'z');
    CHECK(files::pread(fd.value, read.data(), read.size(), 8) == 0);
    CHECK(files::fsync(fd.value) == 0);
    REQUIRE(files::seek(fd.value, 0, SEEK_SET) == 0);
    CHECK(files::read(fd.value, read.data(), read.size()) == data.size());
    CHECK(read[3] == '\x1a');
    CHECK(read[4] == '\0');
    CHECK(read[7] == 'z');
    CHECK(files::pread(fd.value, nullptr, 0, 100) == 0);
    CHECK(files::pwrite(fd.value, nullptr, 0, 100) == 0);
    aio::FileStat info{};
    REQUIRE(files::stat_fd(fd.value, &info) == 0);
    CHECK(info.st_size == 8);
}

TEST_CASE("offsets and descriptor stats retain positions above 4 GiB") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "large.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
#ifdef _WIN32
    // Mark only this test file sparse, so the hole consumes no multi-GiB space.
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(fd.value)),
                            FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                            nullptr));
#endif
    constexpr aio::FileOffset offset = (aio::FileOffset{5} << 30) + 123;
    REQUIRE(files::pwrite(fd.value, "large", 5, offset) == 5);
    aio::FileStat info{};
    REQUIRE(files::stat_fd(fd.value, &info) == 0);
    CHECK(info.st_size == offset + 5);
    std::array<char, 5> read{};
    REQUIRE(files::pread(fd.value, read.data(), read.size(), offset) == 5);
    CHECK(std::string(read.data(), read.size()) == "large");
    REQUIRE(files::seek(fd.value, offset, SEEK_SET) == offset);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == offset);
    CHECK(files::read(fd.value, read.data(), read.size()) == 5);
    CHECK(files::truncate(fd.value, 3) == 0);
    REQUIRE(files::stat_fd(fd.value, &info) == 0);
    CHECK(info.st_size == 3);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == offset + 5);
}

TEST_CASE("concurrent positional operations do not disturb sequential reads") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "concurrent.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    const std::string header(4096, 's');
    REQUIRE(files::write(fd.value, header.data(), header.size()) ==
            header.size());
    REQUIRE(files::seek(fd.value, 0, SEEK_SET) == 0);
    std::atomic<bool> start{false};
    std::atomic<bool> correct{true};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            const std::string data(64, static_cast<char>('A' + worker));
            for (int block = 0; block < 64; ++block) {
                const aio::FileOffset offset =
                    4096 + (worker * 64 + block) * 64;
                std::array<char, 64> read{};
                if (files::pwrite(fd.value, data.data(), data.size(), offset) !=
                        64 ||
                    files::pread(fd.value, read.data(), read.size(), offset) !=
                        64 ||
                    std::string(read.data(), read.size()) != data)
                    correct.store(false, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 512; ++i) {
        char read = 0;
        if (files::read(fd.value, &read, 1) != 1 || read != 's')
            correct = false;
    }
    for (auto& thread : threads) thread.join();
    CHECK(correct.load());
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 512);
}

TEST_CASE("UTF-8 paths can be created and reopened") {
    TemporaryDirectory dir;
    const auto path =
        utf8_path(dir.path / fs::path(u8"\u03bb-\u6d4b\u8bd5.bin"));
    {
        TestFd fd(files::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600));
        REQUIRE(fd.value >= 0);
        REQUIRE(files::write(fd.value, "unicode", 7) == 7);
    }
    TestFd fd(files::open(path.c_str(), O_RDONLY));
    REQUIRE(fd.value >= 0);
    std::array<char, 7> read{};
    REQUIRE(files::pread(fd.value, read.data(), read.size(), 0) == 7);
    CHECK(std::string(read.data(), read.size()) == "unicode");
}

TEST_CASE("positional I/O also supports descriptors owned by stdio") {
    TemporaryDirectory dir;
    const auto path = dir.path / "stdio.bin";
#ifdef _WIN32
    FILE* stream = _wfopen(path.c_str(), L"w+b");
#else
    FILE* stream = std::fopen(path.c_str(), "w+b");
#endif
    REQUIRE(stream != nullptr);
    const auto close_stream = [](FILE* file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_stream)> owner(stream, close_stream);
    REQUIRE(std::fwrite("stdio", 1, 5, stream) == 5);
    REQUIRE(std::fflush(stream) == 0);
#ifdef _WIN32
    const int fd = _fileno(stream);
#else
    const int fd = ::fileno(stream);
#endif
    std::array<char, 5> read{};
    CHECK(files::pread(fd, read.data(), read.size(), 0) == 5);
    CHECK(std::string(read.data(), read.size()) == "stdio");
    CHECK(files::pwrite(fd, "X", 1, 0) == 1);
    CHECK(std::ftell(stream) == 5);
    CHECK(files::seek(fd, 0, SEEK_CUR) == 5);
    owner.reset();
}

TEST_CASE("append and truncate opens preserve their file semantics") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "flags.bin");
    {
        TestFd fd(files::open(path.c_str(), O_CREAT | O_WRONLY, 0600));
        REQUIRE(fd.value >= 0);
        REQUIRE(files::write(fd.value, "first", 5) == 5);
    }
    {
        TestFd fd(files::open(path.c_str(), O_WRONLY | O_APPEND));
        REQUIRE(fd.value >= 0);
        REQUIRE(files::seek(fd.value, 0, SEEK_SET) == 0);
        REQUIRE(files::write(fd.value, "last", 4) == 4);
        aio::FileStat info{};
        REQUIRE(files::stat_fd(fd.value, &info) == 0);
        CHECK(info.st_size == 9);
    }
    {
        TestFd fd(files::open(path.c_str(), O_WRONLY | O_TRUNC));
        REQUIRE(fd.value >= 0);
        aio::FileStat info{};
        REQUIRE(files::stat_fd(fd.value, &info) == 0);
        CHECK(info.st_size == 0);
    }
}

#ifdef _WIN32
TEST_CASE("temporary Windows files are removed when the descriptor closes") {
    TemporaryDirectory dir;
    const auto path = dir.path / "temporary.bin";
    const auto text = utf8_path(path);
    {
        TestFd fd(
            files::open(text.c_str(), O_CREAT | O_RDWR | _O_TEMPORARY, 0600));
        REQUIRE(fd.value >= 0);
        REQUIRE(files::pwrite(fd.value, "temp", 4, 0) == 4);
        CHECK(fs::exists(path));
    }
    CHECK_FALSE(fs::exists(path));
}
#endif

#ifdef _MSC_VER
TEST_CASE("invalid descriptor handling restores the caller's thread handler") {
    const auto handler = +[](const wchar_t*, const wchar_t*, const wchar_t*,
                             unsigned int, std::uintptr_t) {};
    const auto previous = _set_thread_local_invalid_parameter_handler(handler);
    const int result = files::close(-1);
    const int error = errno;
    const auto restored = _set_thread_local_invalid_parameter_handler(previous);
    CHECK(restored == handler);
    CHECK(result == -1);
    CHECK(error == EBADF);
}
#endif

TEST_CASE("missing files, exclusive creation and access errors set errno") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "access.bin");
    errno = 0;
    CHECK(files::open(path.c_str(), O_RDONLY) == -1);
    CHECK(errno == ENOENT);
    {
        TestFd fd(files::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
        REQUIRE(fd.value >= 0);
        REQUIRE(files::write(fd.value, "x", 1) == 1);
        errno = 0;
        CHECK(files::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600) == -1);
        CHECK(errno == EEXIST);
    }
    {
        TestFd fd(files::open(path.c_str(), O_RDONLY));
        REQUIRE(fd.value >= 0);
        errno = 0;
        CHECK(files::pwrite(fd.value, "x", 1, 0) == -1);
        CHECK(errno == EBADF);
    }
    {
        TestFd fd(files::open(path.c_str(), O_WRONLY));
        REQUIRE(fd.value >= 0);
        char read = 0;
        errno = 0;
        CHECK(files::pread(fd.value, &read, 1, 0) == -1);
        CHECK(errno == EBADF);
    }
}

TEST_CASE("invalid descriptors return errors without terminating the process") {
    char byte = 0;
    aio::FileStat info{};
    CHECK(files::read(-1, &byte, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(files::write(-1, &byte, 1) == -1);
    CHECK(errno == EBADF);
    CHECK(files::pread(-1, &byte, 1, 0) == -1);
    CHECK(errno == EBADF);
    CHECK(files::pwrite(-1, &byte, 1, 0) == -1);
    CHECK(errno == EBADF);
    CHECK(files::fsync(-1) == -1);
    CHECK(errno == EBADF);
    CHECK(files::truncate(-1, 0) == -1);
    CHECK(errno == EBADF);
    CHECK(files::seek(-1, 0, SEEK_SET) == -1);
    CHECK(errno == EBADF);
    CHECK(files::stat_fd(-1, &info) == -1);
    CHECK(errno == EBADF);
    CHECK(files::close(-1) == -1);
    CHECK(errno == EBADF);
}

TEST_CASE("positional I/O rejects pipes and negative offsets") {
    int descriptors[2];
#ifdef _WIN32
    REQUIRE(_pipe(descriptors, 4096, _O_BINARY) == 0);
#else
    REQUIRE(::pipe(descriptors) == 0);
#endif
    TestFd reader(descriptors[0]);
    TestFd writer(descriptors[1]);
    char byte = 0;
    CHECK(files::pread(reader.value, &byte, 1, 0) == -1);
    CHECK(errno == ESPIPE);
    CHECK(files::pwrite(writer.value, &byte, 1, 0) == -1);
    CHECK(errno == ESPIPE);
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "offset.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    CHECK(files::pread(fd.value, &byte, 1, -1) == -1);
    CHECK(errno == EINVAL);
    CHECK(files::pwrite(fd.value, &byte, 1, -1) == -1);
    CHECK(errno == EINVAL);
}
