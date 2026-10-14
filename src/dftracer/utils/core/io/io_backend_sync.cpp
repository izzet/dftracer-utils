#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/platform_io.h>

#include <cerrno>

namespace dftracer::utils::io {

IoResult IoBackend::submit_read_sync(int fd, void *buf, std::size_t len,
                                     FileOffset offset) {
    // For sync wrappers, we cannot use the normal IoAwaitable coroutine
    // path. Instead, we directly call the blocking file operation. This
    // synchronous path is used from dedicated blocking contexts, so blocking is
    // acceptable.
    IoResult result = platform_io::pread(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return result;
}

IoResult IoBackend::submit_write_sync(int fd, const void *buf, std::size_t len,
                                      FileOffset offset) {
    IoResult result = platform_io::pwrite(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fsync_sync(int fd) {
    int result = platform_io::fsync(fd);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_ftruncate_sync(int fd, FileOffset length) {
    int result = platform_io::truncate(fd, length);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fstat_sync(int fd, FileStat *buf) {
    int result = platform_io::stat_fd(fd, buf);
    if (result < 0) result = -errno;
    return result;
}

}  // namespace dftracer::utils::io
