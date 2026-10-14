#include <dftracer/utils/core/io/file_ops.h>
#include <dftracer/utils/core/io/io_backend.h>

#include <cerrno>

namespace dftracer::utils::io {

IoResult IoBackend::submit_read_sync(int fd, void *buf, std::size_t len,
                                     FileOffset offset) {
    // For sync wrappers, we cannot use the normal IoAwaitable coroutine
    // path. Instead, we directly call the blocking file operation. This
    // synchronous path is used from dedicated blocking contexts, so blocking is
    // acceptable.
    IoResult result = file_ops::pread(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return result;
}

IoResult IoBackend::submit_write_sync(int fd, const void *buf, std::size_t len,
                                      FileOffset offset) {
    IoResult result = file_ops::pwrite(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fsync_sync(int fd) {
    int result = file_ops::fsync(fd);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_ftruncate_sync(int fd, FileOffset length) {
    int result = file_ops::truncate(fd, length);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fstat_sync(int fd, FileStat *buf) {
    int result = file_ops::stat_fd(fd, buf);
    if (result < 0) result = -errno;
    return result;
}

}  // namespace dftracer::utils::io
