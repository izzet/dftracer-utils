#ifndef DFTRACER_UTILS_CORE_IO_OPS_H
#define DFTRACER_UTILS_CORE_IO_OPS_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/awaitable.h>
#include <dftracer/utils/core/io/types.h>
#include <fcntl.h>

#include <cstddef>

namespace dftracer::utils::io {

/// Asynchronous read (sequential). If inside an executor worker, submits
/// to the I/O backend and suspends. Otherwise falls back to blocking
/// read(). Supports files, pipes and terminals. Windows sockets use recv().
IoAwaitable read(int fd, void* buf, std::size_t len) noexcept;

/// Asynchronous write (sequential). Windows sockets use send().
IoAwaitable write(int fd, const void* buf, std::size_t len) noexcept;

/// Asynchronous positional read. Only works on seekable fds (regular
/// files). Uses pread() under the hood.
IoAwaitable pread(int fd, void* buf, std::size_t len,
                  FileOffset offset) noexcept;

/// Asynchronous positional write. Only works on seekable fds.
IoAwaitable pwrite(int fd, const void* buf, std::size_t len,
                   FileOffset offset) noexcept;

/// Asynchronous open. Returns fd (>= 0) or negative errno.
IoAwaitable open(const char* path, int flags, FileMode mode = 0644) noexcept;

/// Asynchronous close.
IoAwaitable close(int fd) noexcept;

/// Asynchronous fsync.
IoAwaitable fsync(int fd) noexcept;

/// Asynchronous ftruncate.
IoAwaitable ftruncate(int fd, FileOffset length) noexcept;

/// Asynchronous fstat.
IoAwaitable fstat(int fd, FileStat* buf) noexcept;

/// Accept a connection on a listening socket. Returns client fd.
IoAwaitable accept(SocketHandle listen_fd, struct sockaddr* addr = nullptr,
                   SocketLength* addrlen = nullptr) noexcept;

/// Receive data from a connected socket. Returns bytes received.
IoAwaitable recv(SocketHandle fd, void* buf, std::size_t len,
                 int flags = 0) noexcept;

/// Send data to a connected socket. Returns bytes sent.
IoAwaitable send(SocketHandle fd, const void* buf, std::size_t len,
                 int flags = 0) noexcept;

/// Scatter-gather sequential read. Works on any fd type.
IoAwaitable readv(int fd, const IoVec* iov, int iovcnt) noexcept;

/// Scatter-gather sequential write. Works on any fd type. Like writev(2), it
/// accepts at most IOV_MAX entries and may write fewer bytes than requested;
/// use writev_all() unless you handle both.
IoAwaitable writev(int fd, const IoVec* iov, int iovcnt) noexcept;

/// writev() until every byte is sent: splits runs longer than IOV_MAX and
/// resumes after short writes. `iov` is mutated as entries are consumed.
/// Returns the bytes written, or a negative errno on failure.
coro::CoroTask<IoResult> writev_all(int fd, IoVec* iov, int iovcnt);

/// Scatter-gather positional read. Only works on seekable fds.
IoAwaitable preadv(int fd, const IoVec* iov, int iovcnt,
                   FileOffset offset) noexcept;

/// Scatter-gather positional write. Only works on seekable fds.
IoAwaitable pwritev(int fd, const IoVec* iov, int iovcnt,
                    FileOffset offset) noexcept;

/// Reposition file offset. Returns new offset or negative errno.
IoAwaitable lseek(int fd, FileOffset offset, int whence) noexcept;

/// Zero-copy transfer from in_fd to out_fd. Returns bytes sent.
/// in_fd must be a regular file. Starts at the given offset.
IoAwaitable sendfile(int out_fd, int in_fd, FileOffset offset,
                     std::size_t count) noexcept;

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_OPS_H
