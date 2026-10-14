#ifndef DFTRACER_UTILS_CORE_IO_FILE_OPS_H
#define DFTRACER_UTILS_CORE_IO_FILE_OPS_H

#include <dftracer/utils/core/io/types.h>
#include <fcntl.h>

namespace dftracer::utils::io::file_ops {

// Blocking file operations for synchronous callers and thread-pool backends.
// Failures return -1 and set errno, matching the POSIX calls they replace.
// Paths use UTF-8 on Windows; files opened here use binary I/O there.
int open(const char* path, int flags, FileMode mode = 0644) noexcept;
int close(int fd) noexcept;
IoResult read(int fd, void* buf, std::size_t len) noexcept;
IoResult write(int fd, const void* buf, std::size_t len) noexcept;

// Positional I/O never changes the descriptor's sequential file position and
// supports concurrent operations on one descriptor at independent offsets.
IoResult pread(int fd, void* buf, std::size_t len, FileOffset offset) noexcept;
IoResult pwrite(int fd, const void* buf, std::size_t len,
                FileOffset offset) noexcept;

int fsync(int fd) noexcept;
int truncate(int fd, FileOffset length) noexcept;
FileOffset seek(int fd, FileOffset offset, int whence) noexcept;
int stat_fd(int fd, FileStat* buf) noexcept;

}  // namespace dftracer::utils::io::file_ops

#endif  // DFTRACER_UTILS_CORE_IO_FILE_OPS_H
