#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/common/scratch.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <dftracer/utils/core/common/symbolize.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
namespace aio = dftracer::utils::io;

namespace {
class TestDirectory {
   public:
    TestDirectory() {
        static std::atomic<unsigned> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("dftu_runtime_" + std::to_string(stamp) + "_" +
                std::to_string(counter++));
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("cannot create runtime test directory");
    }
    ~TestDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

class EnvironmentValue {
   public:
    EnvironmentValue(const char* key, const std::string& value) : key_(key) {
        if (const char* old = std::getenv(key)) previous_ = old;
        set(value.c_str());
    }
    ~EnvironmentValue() { set(previous_ ? previous_->c_str() : nullptr); }

   private:
    void set(const char* value) {
#ifdef _WIN32
        ::_putenv_s(key_, value ? value : "");
#else
        if (value)
            ::setenv(key_, value, 1);
        else
            ::unsetenv(key_);
#endif
    }
    const char* key_;
    std::optional<std::string> previous_;
};
}  // namespace

TEST_CASE("thread-pool runtime performs concurrent positional and vector I/O") {
    TestDirectory dir;
    const auto path =
        utf8(dir.path / std::filesystem::path(u8"\u03bb-runtime.bin"));
    for (unsigned batch : {0u, 16u}) {
        Runtime runtime(
            ExecutorConfig{.num_threads = 2,
                           .io_pool_size = 2,
                           .io_backend_type = aio::IoBackendType::THREADPOOL,
                           .io_batch_threshold = batch});
        std::atomic<bool> correct{true};
        runtime.run_blocking(
            "platform_io", [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto request =
                    aio::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
                CHECK_FALSE(request.await_ready());
                const auto opened = co_await request;
                REQUIRE(opened >= 0);
                ScopedFd fd(static_cast<int>(opened));
                char a[] = "abc";
                char b[] = "def";
                aio::IoVec vectors[]{{a, 3}, {b, 3}};
                CHECK(co_await aio::writev_all(fd.get(), vectors, 2) == 6);
                for (int worker = 0; worker < 16; ++worker) {
                    scope.spawn(
                        [&, worker](CoroScope&) -> coro::CoroTask<void> {
                            const std::string data(
                                64, static_cast<char>('A' + worker));
                            const aio::FileOffset offset = 64 + worker * 64;
                            std::array<char, 64> back{};
                            const auto w = co_await aio::pwrite(
                                fd.get(), data.data(), data.size(), offset);
                            if (w != 64) correct = false;
                            const auto r = co_await aio::pread(
                                fd.get(), back.data(), back.size(), offset);
                            if (r != 64 ||
                                std::string(back.data(), back.size()) != data)
                                correct = false;
                        });
                }
                co_await scope.join();
                CHECK(co_await aio::lseek(fd.get(), 0, SEEK_CUR) == 6);
                aio::FileStat info{};
                CHECK(co_await aio::fstat(fd.get(), &info) == 0);
                CHECK(info.st_size == 64 + 16 * 64);
                CHECK(co_await aio::fsync(fd.get()) == 0);
                CHECK(co_await aio::ftruncate(fd.get(), 6) == 0);
                std::array<char, 6> back{};
                CHECK(co_await aio::pread(fd.get(), back.data(), back.size(),
                                          0) == 6);
                CHECK(std::string(back.data(), back.size()) == "abcdef");
                CHECK(co_await aio::pread(-1, back.data(), 1, 0) == -EBADF);
                CHECK(co_await aio::close(fd.get()) == 0);
                fd.value = -1;
            });
        CHECK(correct.load());
    }
}

TEST_CASE("spill files support concurrent appends and are removed on close") {
    TestDirectory dir;
    EnvironmentValue root("DFTRACER_UTILS_SPILL_DIR", utf8(dir.path));
    auto created = SpillFile::create();
    REQUIRE(created.has_value());
    auto file = std::move(*created);
    constexpr int WORKERS = 8;
    std::array<std::uint64_t, WORKERS> offsets{};
    std::atomic<bool> correct{true};
    std::vector<std::thread> threads;
    for (int i = 0; i < WORKERS; ++i) {
        threads.emplace_back([&, i] {
            const std::string data(4096, static_cast<char>('A' + i));
            auto offset = file->append(data.data(), data.size());
            if (!offset) {
                correct = false;
                return;
            }
            offsets[i] = *offset;
            std::array<char, 4096> back{};
            if (!file->read(*offset, back.data(), back.size()) ||
                std::string(back.data(), back.size()) != data)
                correct = false;
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(correct.load());
    CHECK(file->size() == WORKERS * 4096);
    for (int i = 0; i < WORKERS; ++i)
        for (int j = i + 1; j < WORKERS; ++j) CHECK(offsets[i] != offsets[j]);
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(file->fd()));
    std::wstring name(32768, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(
        handle, name.data(), static_cast<DWORD>(name.size()), 0);
    REQUIRE(size > 0);
    REQUIRE(size < name.size());
    name.resize(size);
    CHECK(std::filesystem::exists(name));
    file.reset();
    CHECK_FALSE(std::filesystem::exists(name));
#endif
}

TEST_CASE("scratch and spill cleanup retain live and other-host directories") {
    TestDirectory dir;
    EnvironmentValue scratch_root("DFTRACER_INDEX_SCRATCH", utf8(dir.path));
    std::string scratch;
    {
        ScratchSession session;
        REQUIRE(session.valid());
        scratch = session.dir();
        CHECK(std::filesystem::is_directory(scratch));
    }
    CHECK_FALSE(std::filesystem::exists(scratch));

    EnvironmentValue spill_root("DFTRACER_UTILS_SPILL_DIR", utf8(dir.path));
    char host[256]{};
#ifdef _WIN32
    DWORD size = sizeof(host);
    REQUIRE(GetComputerNameA(host, &size));
#else
    REQUIRE(::gethostname(host, sizeof(host) - 1) == 0);
#endif
    std::string tag(host);
    for (char& c : tag)
        if (c == '/' || c == '_') c = '-';
    const std::string prefix = "platform_sweep";
    const auto dead = dir.path / (prefix + "_" + tag + "_2147483646_0");
    const auto live = dir.path / (prefix + "_" + tag + "_" +
                                  std::to_string(process_id()) + "_old");
    const auto other = dir.path / (prefix + "_another-host_2147483646_0");
    REQUIRE(std::filesystem::create_directory(dead));
    REQUIRE(std::filesystem::create_directory(live));
    REQUIRE(std::filesystem::create_directory(other));
    auto subdir = ScopedSpillSubdir::create(prefix);
    REQUIRE(subdir.has_value());
    CHECK_FALSE(std::filesystem::exists(dead));
    CHECK(std::filesystem::exists(live));
    CHECK(std::filesystem::exists(other));
}

TEST_CASE("filesystem probes and symbol lookup work on the native platform") {
    TestDirectory dir;
    const auto path = utf8(dir.path);
    CHECK(filesystem_kind(path) != FilesystemKind::UNKNOWN);
    CHECK_FALSE(find_mountpoint(path).empty());
    CHECK(find_mountpoint(utf8(dir.path / "missing" / "nested")) ==
          find_mountpoint(path));
    CHECK_FALSE(list_mounts().empty());
    CHECK(is_network_filesystem(FilesystemKind::SMB));
    CHECK(symbolize_function(nullptr) == "?");
    CHECK(symbolize_function(reinterpret_cast<const void*>(&filesystem_kind)) !=
          "?");
}
