#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <dftracer/utils/core/io/types.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

// Platform-specific includes for file stats
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <fcntl.h>

#include <string_view>

namespace dftracer::utils::index::store::internal {
namespace {
std::string path_utf8(const fs::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
}  // namespace

// Registry key: the canonical absolute path, so same-named files in different
// directories are distinct and the key locates the trace. Applied on both the
// write and read side so lookups stay consistent; relative inputs resolve
// against the cwd, so the index is not portable across an absolute-layout move.
std::string get_logical_path(std::string_view path) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::u8path(path.begin(), path.end()), ec);
    if (ec)
        return path_utf8(
            fs::u8path(path.begin(), path.end()).lexically_normal());
    return path_utf8(abs.lexically_normal());
}

std::string normalize_index_root(std::string_view path) {
    fs::path input = fs::u8path(path.begin(), path.end());
    if (input.filename() == ".dftindex") {
        return path_utf8(input);
    }
    if (input.parent_path().filename() == ".dftindex") {
        return path_utf8(input.parent_path());
    }
    if (input.extension() == ".idx" || input.extension() == ".pidx" ||
        input.has_extension()) {
        return path_utf8(input.parent_path() / ".dftindex");
    }
    return path_utf8(input / ".dftindex");
}

time_t get_file_modification_time(const std::string &file_path) {
    std::error_code ec;
    auto ftime = fs::last_write_time(fs::u8path(file_path), ec);
    if (ec) return 0;
    return dftracer::utils::file_mtime_seconds(ftime);
}

std::uint64_t calculate_file_hash(const std::string &file_path) {
    // Fast fingerprint: hash file_size + first 64KB + last 64KB.
    constexpr std::size_t SAMPLE_SIZE = 64 * 1024;

    int fd = io::platform_io::open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for hash calculation: %s",
                                 file_path.c_str());
        return 0;
    }

    io::FileStat st{};
    if (io::platform_io::stat_fd(fd, &st) != 0) {
        io::platform_io::close(fd);
        return 0;
    }
    auto file_size = static_cast<std::size_t>(st.st_size);

    static thread_local dftracer::utils::utilities::hash::HasherUtility hasher;
    hasher.reset();

    // Hash the file size itself
    char size_buf[32];
    int n = std::snprintf(size_buf, sizeof(size_buf), "%zu", file_size);
    hasher.update(std::string_view(size_buf, static_cast<std::size_t>(n)));

    unsigned char buf[SAMPLE_SIZE];

    // Hash first SAMPLE_SIZE bytes
    std::size_t head_bytes = std::min(file_size, SAMPLE_SIZE);
    io::IoResult rd = io::platform_io::pread(fd, buf, head_bytes, 0);
    if (rd > 0) {
        hasher.update(std::string_view(reinterpret_cast<const char *>(buf),
                                       static_cast<std::size_t>(rd)));
    }

    // Hash last SAMPLE_SIZE bytes, only if file is large enough such that they
    // don't overlap with the head
    if (file_size > SAMPLE_SIZE) {
        io::FileOffset tail_off = static_cast<io::FileOffset>(
            file_size - std::min(file_size, SAMPLE_SIZE));
        rd = io::platform_io::pread(fd, buf, SAMPLE_SIZE, tail_off);
        if (rd > 0) {
            hasher.update(std::string_view(reinterpret_cast<const char *>(buf),
                                           static_cast<std::size_t>(rd)));
        }
    }

    io::platform_io::close(fd);
    return static_cast<std::uint64_t>(hasher.get_hash().value);
}

std::uint64_t file_size_bytes(const std::string &path) {
    io::FileStat st{};
    if (io::platform_io::stat_path(path.c_str(), &st) == 0) {
#if defined(_WIN32)
        if ((st.st_mode & _S_IFREG) != 0)
            return static_cast<std::uint64_t>(st.st_size);
#else
        if (S_ISREG(st.st_mode)) return static_cast<std::uint64_t>(st.st_size);
#endif
    }

    int fd = io::platform_io::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    io::FileOffset pos = io::platform_io::seek(fd, 0, SEEK_END);
    io::platform_io::close(fd);
    if (pos < 0) return 0;
    return static_cast<std::uint64_t>(pos);
}

}  // namespace dftracer::utils::index::store::internal
