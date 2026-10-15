#ifndef DFTRACER_UTILS_CORE_IO_PLATFORM_IO_H
#define DFTRACER_UTILS_CORE_IO_PLATFORM_IO_H

#include <dftracer/utils/core/io/types.h>
#include <fcntl.h>

namespace dftracer::utils::io::platform_io {

// Internal blocking I/O implementation shared by the facade and backends.
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

int iov_max_entries() noexcept;
IoResult readv(int fd, const IoVec* iov, int count) noexcept;
IoResult writev(int fd, const IoVec* iov, int count) noexcept;
IoResult preadv(int fd, const IoVec* iov, int count,
                FileOffset offset) noexcept;
IoResult pwritev(int fd, const IoVec* iov, int count,
                 FileOffset offset) noexcept;

// Socket failures also set errno, translating Winsock errors on Windows.
// The caller owns socket initialization and closes sockets with closesocket
// on Windows. File descriptors and socket handles are separate domains there.
IoResult accept(SocketHandle socket, sockaddr* addr, SocketLength* addrlen,
                bool nonblocking = false) noexcept;
IoResult recv(SocketHandle socket, void* buf, std::size_t len,
              int flags) noexcept;
IoResult send(SocketHandle socket, const void* buf, std::size_t len,
              int flags) noexcept;

}  // namespace dftracer::utils::io::platform_io

#endif  // DFTRACER_UTILS_CORE_IO_PLATFORM_IO_H
