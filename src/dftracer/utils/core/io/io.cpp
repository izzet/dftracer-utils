#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/io_sendfile.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <dftracer/utils/core/pipeline/executor.h>

#include <algorithm>
#include <cerrno>
#include <climits>

namespace dftracer::utils::io {

namespace {

// Every free op has the same shape: if the current executor has an I/O backend,
// forward to its submit_* member (async); otherwise run the blocking syscall,
// fold a failure to -errno, and hand back a ready awaitable. Submit is the
// backend member selected as a template argument; sync is the syscall wrapper.
template <auto Submit, class Sync, class... Args>
IoAwaitable io_dispatch(Sync sync, Args... args) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return (exec->io_backend().*Submit)(args...);
    }
    auto result = sync(args...);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<IoResult>(result));
}

}  // namespace

IoAwaitable read(int fd, void* buf, std::size_t len) noexcept {
    return io_dispatch<&IoBackend::submit_read>(
        [](int f, void* b, std::size_t n) {
            return platform_io::read(f, b, n);
        },
        fd, buf, len);
}

IoAwaitable write(int fd, const void* buf, std::size_t len) noexcept {
    return io_dispatch<&IoBackend::submit_write>(
        [](int f, const void* b, std::size_t n) {
            return platform_io::write(f, b, n);
        },
        fd, buf, len);
}

IoAwaitable pread(int fd, void* buf, std::size_t len,
                  FileOffset offset) noexcept {
    return io_dispatch<&IoBackend::submit_pread>(
        [](int f, void* b, std::size_t n, FileOffset o) {
            return platform_io::pread(f, b, n, o);
        },
        fd, buf, len, offset);
}

IoAwaitable pwrite(int fd, const void* buf, std::size_t len,
                   FileOffset offset) noexcept {
    return io_dispatch<&IoBackend::submit_pwrite>(
        [](int f, const void* b, std::size_t n, FileOffset o) {
            return platform_io::pwrite(f, b, n, o);
        },
        fd, buf, len, offset);
}

IoAwaitable open(const char* path, int flags, FileMode mode) noexcept {
    return io_dispatch<&IoBackend::submit_open>(
        [](const char* p, int fl, FileMode m) {
            return platform_io::open(p, fl, m);
        },
        path, flags, mode);
}

IoAwaitable close(int fd) noexcept {
    return io_dispatch<&IoBackend::submit_close>(
        [](int f) { return platform_io::close(f); }, fd);
}

IoAwaitable fsync(int fd) noexcept {
    return io_dispatch<&IoBackend::submit_fsync>(
        [](int f) { return platform_io::fsync(f); }, fd);
}

IoAwaitable ftruncate(int fd, FileOffset length) noexcept {
    return io_dispatch<&IoBackend::submit_ftruncate>(
        [](int f, FileOffset l) { return platform_io::truncate(f, l); }, fd,
        length);
}

IoAwaitable fstat(int fd, FileStat* buf) noexcept {
    return io_dispatch<&IoBackend::submit_fstat>(
        [](int f, FileStat* b) { return platform_io::stat_fd(f, b); }, fd, buf);
}

IoAwaitable accept(SocketHandle listen_fd, struct sockaddr* addr,
                   SocketLength* addrlen) noexcept {
    return io_dispatch<&IoBackend::submit_accept>(
        [](SocketHandle lf, struct sockaddr* a, SocketLength* al) {
            return platform_io::accept(lf, a, al);
        },
        listen_fd, addr, addrlen);
}

IoAwaitable recv(SocketHandle fd, void* buf, std::size_t len,
                 int flags) noexcept {
    return io_dispatch<&IoBackend::submit_recv>(
        [](SocketHandle f, void* b, std::size_t n, int fl) {
            return platform_io::recv(f, b, n, fl);
        },
        fd, buf, len, flags);
}

IoAwaitable send(SocketHandle fd, const void* buf, std::size_t len,
                 int flags) noexcept {
    return io_dispatch<&IoBackend::submit_send>(
        [](SocketHandle f, const void* b, std::size_t n, int fl) {
            return platform_io::send(f, b, n, fl);
        },
        fd, buf, len, flags);
}

IoAwaitable readv(int fd, const IoVec* iov, int iovcnt) noexcept {
    return io_dispatch<&IoBackend::submit_readv>(
        [](int f, const IoVec* v, int c) {
            return platform_io::readv(f, v, c);
        },
        fd, iov, iovcnt);
}

IoAwaitable writev(int fd, const IoVec* iov, int iovcnt) noexcept {
    return io_dispatch<&IoBackend::submit_writev>(
        [](int f, const IoVec* v, int c) {
            return platform_io::writev(f, v, c);
        },
        fd, iov, iovcnt);
}

coro::CoroTask<IoResult> writev_all(int fd, IoVec* iov, int iovcnt) {
    IoResult total = 0;
    int i = 0;
    while (i < iovcnt) {
        int n = std::min(iovcnt - i, platform_io::iov_max_entries());
        IoResult rc = co_await io::writev(fd, iov + i, n);
        if (rc < 0) co_return rc;
        if (rc == 0) co_return -EIO;  // no progress; avoid spinning
        total += rc;

        auto written = static_cast<std::size_t>(rc);
        while (i < iovcnt && written > 0) {
            if (written >= iov[i].iov_len) {
                written -= iov[i].iov_len;
                ++i;
            } else {
                iov[i].iov_base = static_cast<char*>(iov[i].iov_base) + written;
                iov[i].iov_len -= written;
                written = 0;
            }
        }
    }
    co_return total;
}

IoAwaitable preadv(int fd, const IoVec* iov, int iovcnt,
                   FileOffset offset) noexcept {
    return io_dispatch<&IoBackend::submit_preadv>(
        [](int f, const IoVec* v, int c, FileOffset o) {
            return platform_io::preadv(f, v, c, o);
        },
        fd, iov, iovcnt, offset);
}

IoAwaitable pwritev(int fd, const IoVec* iov, int iovcnt,
                    FileOffset offset) noexcept {
    return io_dispatch<&IoBackend::submit_pwritev>(
        [](int f, const IoVec* v, int c, FileOffset o) {
            return platform_io::pwritev(f, v, c, o);
        },
        fd, iov, iovcnt, offset);
}

IoAwaitable lseek(int fd, FileOffset offset, int whence) noexcept {
    return io_dispatch<&IoBackend::submit_lseek>(
        [](int f, FileOffset o, int w) { return platform_io::seek(f, o, w); },
        fd, offset, whence);
}

IoAwaitable sendfile(int out_fd, int in_fd, FileOffset offset,
                     std::size_t count) noexcept {
    return io_dispatch<&IoBackend::submit_sendfile>(
        [](int of, int inf, FileOffset o, std::size_t c) {
            return platform_sendfile(of, inf, o, c);
        },
        out_fd, in_fd, offset, count);
}

}  // namespace dftracer::utils::io
