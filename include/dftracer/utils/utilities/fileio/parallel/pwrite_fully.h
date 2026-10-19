#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PWRITE_FULLY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PWRITE_FULLY_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/io/types.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::utilities::fileio::parallel {

/// Drain a full buffer to fd at the given absolute offset via pwrite, retrying
/// short writes. Returns 0 on success, -1 on error (logged). path is used only
/// for the error message.
inline coro::CoroTask<int> pwrite_fully(int fd, const char* path,
                                        const std::uint8_t* bytes,
                                        std::size_t size,
                                        io::FileOffset offset) {
    std::size_t written = 0;
    while (written < size) {
        auto n =
            co_await io::pwrite(fd, bytes + written, size - written,
                                offset + static_cast<io::FileOffset>(written));
        if (n <= 0) {
            DFTRACER_UTILS_LOG_ERROR("pwrite failed on %s at offset %lld", path,
                                     static_cast<long long>(offset));
            co_return -1;
        }
        written += static_cast<std::size_t>(n);
    }
    co_return 0;
}

}  // namespace dftracer::utils::utilities::fileio::parallel

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PWRITE_FULLY_H
