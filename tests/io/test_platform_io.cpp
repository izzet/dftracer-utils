#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/io/awaitable.h>
#include <dftracer/utils/core/io/io_sendfile.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

// Requires the Win32 declarations provided by Winsock's Windows include.
#include <io.h>
#include <winioctl.h>
#else
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace aio = dftracer::utils::io;
namespace files = dftracer::utils::io::platform_io;
namespace fs = std::filesystem;

#ifdef _WIN32
static_assert(std::is_same_v<aio::SocketHandle, SOCKET>);
#endif

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
               ("dftu_platform_io_" + std::to_string(stamp) + "_" +
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
    aio::IoVec parts[]{{read.data(), 2}, {read.data() + 2, 3}};
    REQUIRE(files::pwritev(fd.value, parts, 2, offset) == 5);
    read.fill(0);
    REQUIRE(files::preadv(fd.value, parts, 2, offset) == 5);
    CHECK(std::string(read.data(), read.size()) == "large");
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 0);
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

TEST_CASE(
    "vectored I/O preserves ordering, short reads and sequential position") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "vectors.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    char first[] = "abc";
    char second[] = "defg";
    aio::IoVec output[]{{first, 3}, {nullptr, 0}, {second, 4}};
    REQUIRE(files::writev(fd.value, output, 3) == 7);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 7);
    REQUIRE(files::pwritev(fd.value, output, 3, 7) == 7);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 7);
    char a[4]{};
    char b[6]{};
    aio::IoVec input[]{{a, sizeof(a)}, {nullptr, 0}, {b, sizeof(b)}};
    REQUIRE(files::preadv(fd.value, input, 3, 8) == 6);
    CHECK(std::string(a, 4) == "bcde");
    CHECK(std::string(b, 2) == "fg");
    CHECK(b[2] == 0);
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 7);
    REQUIRE(files::readv(fd.value, input, 3) == 7);
    CHECK(std::string(a, 4) == "abcd");
    CHECK(std::string(b, 3) == "efg");
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 14);
    CHECK(files::readv(fd.value, nullptr, 0) == 0);
    CHECK(files::writev(fd.value, nullptr, 0) == 0);
    CHECK(files::preadv(fd.value, input, 3, 14) == 0);
    CHECK(files::readv(fd.value, input, -1) == -1);
    CHECK(errno == EINVAL);
    CHECK(files::writev(fd.value, input, files::iov_max_entries() + 1) == -1);
    CHECK(errno == EINVAL);
    CHECK(files::preadv(fd.value, input, 3, -1) == -1);
    CHECK(errno == EINVAL);
    CHECK(files::writev(-1, output, 3) == -1);
    CHECK(errno == EBADF);
#ifdef _WIN32
    // Validate the entire request before copying or submitting anything.
    CHECK(files::readv(fd.value, nullptr, 1) == -1);
    CHECK(errno == EFAULT);
    aio::IoVec overflow[]{
        {first,
         static_cast<std::size_t>(std::numeric_limits<aio::IoResult>::max())},
        {second, 1}};
    CHECK(files::writev(fd.value, overflow, 2) == -1);
    CHECK(errno == EINVAL);
#endif
}

#ifndef __APPLE__
// macOS's native sendfile requires a socket destination, unlike Linux and
// the Windows descriptor fallback.
TEST_CASE("sendfile copies a range into a pipe without moving the input") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "sendfile.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    REQUIRE(files::write(fd.value, "0123456789", 10) == 10);
    int descriptors[2];
#ifdef _WIN32
    REQUIRE(_pipe(descriptors, 4096, _O_BINARY) == 0);
#else
    REQUIRE(::pipe(descriptors) == 0);
#endif
    TestFd reader(descriptors[0]);
    TestFd writer(descriptors[1]);
    REQUIRE(aio::platform_sendfile(writer.value, fd.value, 2, 5) == 5);
    char data[5]{};
    REQUIRE(files::read(reader.value, data, sizeof(data)) == 5);
    CHECK(std::string(data, 5) == "23456");
    CHECK(files::seek(fd.value, 0, SEEK_CUR) == 10);
    CHECK(aio::platform_sendfile(writer.value, fd.value, 10, 5) == 0);
}
#endif

namespace {

class SocketSession {
   public:
    SocketSession() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("cannot initialize Winsock");
#endif
    }
    ~SocketSession() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

class TestSocket {
   public:
    explicit TestSocket(aio::SocketHandle socket) : value(socket) {}
    ~TestSocket() {
#ifdef _WIN32
        if (value != INVALID_SOCKET) ::closesocket(value);
#else
        if (value >= 0) ::close(value);
#endif
    }
    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;
    aio::SocketHandle value;
};

}  // namespace

TEST_CASE("socket wrappers accept, exchange bytes and translate errors") {
    SocketSession session;
    TestSocket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    REQUIRE(listener.value != static_cast<aio::SocketHandle>(-1));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listener.value, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(listener.value, 1) == 0);
    aio::SocketLength size = sizeof(address);
    REQUIRE(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address),
                          &size) == 0);
    TestSocket client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    REQUIRE(client.value != static_cast<aio::SocketHandle>(-1));
    REQUIRE(::connect(client.value, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0);
    const auto result = files::accept(listener.value, nullptr, nullptr);
    REQUIRE(result >= 0);
    TestSocket peer(static_cast<aio::SocketHandle>(result));
    // Bound receive waits so a regression cannot hang the test process.
#ifdef _WIN32
    DWORD timeout = 5000;
    REQUIRE(::setsockopt(peer.value, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout)) == 0);
#else
    timeval timeout{5, 0};
    REQUIRE(::setsockopt(peer.value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0);
#endif
    REQUIRE(files::send(client.value, "hello", 5, 0) == 5);
    char data[5]{};
    REQUIRE(files::recv(peer.value, data, 5, MSG_WAITALL) == 5);
    CHECK(std::string(data, 5) == "hello");
#ifdef _WIN32
    DWORD handle_flags = HANDLE_FLAG_INHERIT;
    REQUIRE(GetHandleInformation(reinterpret_cast<HANDLE>(peer.value),
                                 &handle_flags));
    CHECK((handle_flags & HANDLE_FLAG_INHERIT) == 0);
    u_long mode = 1;
    REQUIRE(::ioctlsocket(peer.value, FIONBIO, &mode) == 0);
    errno = 0;
    CHECK(files::recv(peer.value, data, 1, 0) == -1);
    CHECK(errno == EAGAIN);
    errno = 0;
    CHECK(files::send(INVALID_SOCKET, "x", 1, 0) == -1);
    CHECK(errno == ENOTSOCK);
    errno = 0;
    CHECK(files::accept(INVALID_SOCKET, nullptr, nullptr) == -1);
    CHECK(errno == ENOTSOCK);
#endif
}

TEST_CASE(
    "read-only file mappings outlive their descriptor and validate alignment") {
    TemporaryDirectory dir;
    const auto path = utf8_path(dir.path / "mapped.bin");
    TestFd fd(files::open(path.c_str(), O_CREAT | O_RDWR, 0600));
    REQUIRE(fd.value >= 0);
    const auto granularity = files::mapping_granularity();
    REQUIRE(granularity > 0);
    REQUIRE(files::truncate(
                fd.value, static_cast<aio::FileOffset>(granularity * 2)) == 0);
    REQUIRE(files::pwrite(fd.value, "mapped", 6,
                          static_cast<aio::FileOffset>(granularity)) == 6);
    CHECK(files::map_readonly(fd.value, 1, 1) == nullptr);
    CHECK(errno == EINVAL);
    CHECK(files::map_readonly(fd.value, 0, 0) == nullptr);
    CHECK(errno == EINVAL);
    CHECK(files::map_readonly(-1, 0, granularity) == nullptr);
    CHECK(errno == EBADF);
    void* address = files::map_readonly(
        fd.value, static_cast<aio::FileOffset>(granularity), granularity);
    REQUIRE(address != nullptr);
    const auto release = [granularity](void* p) {
        files::unmap(p, granularity);
    };
    const std::unique_ptr<void, decltype(release)> mapped(address, release);
    REQUIRE(files::close(fd.value) == 0);
    fd.value = -1;
    CHECK(std::string(static_cast<const char*>(address), 6) == "mapped");
}
