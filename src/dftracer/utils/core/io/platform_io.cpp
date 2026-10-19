#include <dftracer/utils/core/io/io_sendfile.h>
#include <dftracer/utils/core/io/platform_io.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <stdlib.h>
#include <winsock2.h>

#include <bit>
#include <new>
#include <string>
#include <vector>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dftracer::utils::io::platform_io {

#ifdef _WIN32
namespace {

// Invalid descriptors must report EBADF rather than invoke the CRT's default
// process-terminating invalid-parameter handler. Restore the caller's handler
// on exit, and change only this thread's handler.
class InvalidParameterScope {
   public:
    InvalidParameterScope()
        : setter_(setter()), previous_(setter_ ? setter_(ignore) : nullptr) {}
    ~InvalidParameterScope() {
        if (setter_) setter_(previous_);
    }

   private:
    static void ignore(const wchar_t*, const wchar_t*, const wchar_t*,
                       unsigned int, std::uintptr_t) {}
    using SetHandler =
        _invalid_parameter_handler(__cdecl*)(_invalid_parameter_handler);
    static SetHandler setter() {
#ifdef _MSC_VER
        return _set_thread_local_invalid_parameter_handler;
#elif defined(_UCRT)
        // Older MinGW headers/import libraries omit this UCRT entry point.
        static const auto function = std::bit_cast<SetHandler>(
            GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"),
                           "_set_thread_local_invalid_parameter_handler"));
        return function;
#else
        // The legacy MinGW CRT does not have a thread-local handler.
        return nullptr;
#endif
    }
    SetHandler setter_;
    _invalid_parameter_handler previous_;
};

class ScopedHandle {
   public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr)
            CloseHandle(handle_);
    }
    HANDLE get() const { return handle_; }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

   private:
    HANDLE handle_;
};

int win_error(DWORD error) noexcept {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            errno = EACCES;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_INVALID_HANDLE:
            errno = EBADF;
            break;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
            errno = EINVAL;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_NOT_ENOUGH_QUOTA:
            errno = ENOMEM;
            break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            errno = ENOSPC;
            break;
        case ERROR_FILENAME_EXCED_RANGE:
            errno = ENAMETOOLONG;
            break;
        case ERROR_NO_UNICODE_TRANSLATION:
            errno = EILSEQ;
            break;
        case ERROR_OPERATION_ABORTED:
            errno = EINTR;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}

IoResult positional_io(int fd, void* buf, std::size_t len, FileOffset offset,
                       bool writing) noexcept {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (buf == nullptr && len != 0) {
        errno = EFAULT;
        return -1;
    }
    InvalidParameterScope parameters;
    const auto raw = _get_osfhandle(fd);
    if (raw == -1 || raw == -2) {
        errno = EBADF;
        return -1;
    }
    const auto original = reinterpret_cast<HANDLE>(raw);
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(original);
    if (type != FILE_TYPE_DISK) {
        const DWORD error = GetLastError();
        if (type == FILE_TYPE_UNKNOWN && error != ERROR_SUCCESS)
            return win_error(error);
        errno = ESPIPE;
        return -1;
    }

    // ReOpenFile can request different access rights. Check access on the
    // original descriptor first, without transferring bytes or moving it.
    char probe = 0;
    DWORD transferred = 0;
    const BOOL access =
        writing ? WriteFile(original, &probe, 0, &transferred, nullptr)
                : ReadFile(original, &probe, 0, &transferred, nullptr);
    if (!access) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            errno = EBADF;
            return -1;
        }
        if (error != ERROR_HANDLE_EOF) return win_error(error);
    }
    if (len == 0) return 0;

    // An OVERLAPPED offset on a synchronous handle still moves its pointer.
    // Reopen as overlapped so each operation has an independent file position.
    ScopedHandle file(
        ReOpenFile(original, writing ? GENERIC_WRITE : GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_FLAG_OVERLAPPED));
    if (file.get() == INVALID_HANDLE_VALUE) return win_error(GetLastError());
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) return win_error(GetLastError());
    OVERLAPPED operation{};
    operation.Offset = static_cast<DWORD>(offset);
    operation.OffsetHigh =
        static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
    operation.hEvent = event.get();
    const DWORD count = static_cast<DWORD>(std::min(
        len, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    const BOOL completed =
        writing ? WriteFile(file.get(), buf, count, nullptr, &operation)
                : ReadFile(file.get(), buf, count, nullptr, &operation);
    if (!completed) {
        const DWORD error = GetLastError();
        if (!writing && error == ERROR_HANDLE_EOF) return 0;
        if (error != ERROR_IO_PENDING) return win_error(error);
    }
    if (!GetOverlappedResult(file.get(), &operation, &transferred, TRUE)) {
        const DWORD error = GetLastError();
        if (!writing && error == ERROR_HANDLE_EOF) return 0;
        return win_error(error);
    }
    return static_cast<IoResult>(transferred);
}

}  // namespace
#endif

int open(const char* path, int flags, FileMode mode) noexcept {
#ifdef _WIN32
    if (path == nullptr) {
        errno = EFAULT;
        return -1;
    }
    const int access = flags & (_O_WRONLY | _O_RDWR);
    if (access == (_O_WRONLY | _O_RDWR) ||
        ((flags & _O_TRUNC) && access == _O_RDONLY)) {
        errno = EINVAL;
        return -1;
    }
    try {
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             path, -1, nullptr, 0);
        if (size == 0) return win_error(GetLastError());
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                 wide.data(), size))
            return win_error(GetLastError());
        DWORD desired = access == _O_RDWR     ? GENERIC_READ | GENERIC_WRITE
                        : access == _O_WRONLY ? GENERIC_WRITE
                                              : GENERIC_READ;
        DWORD creation = OPEN_EXISTING;
        if (flags & _O_CREAT) {
            creation = flags & _O_EXCL    ? CREATE_NEW
                       : flags & _O_TRUNC ? CREATE_ALWAYS
                                          : OPEN_ALWAYS;
        } else if (flags & _O_TRUNC) {
            creation = TRUNCATE_EXISTING;
        }
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        if ((flags & _O_CREAT) && !(mode & _S_IWRITE))
            attributes = FILE_ATTRIBUTE_READONLY;
        if (flags & _O_TEMPORARY) {
            attributes |= FILE_FLAG_DELETE_ON_CLOSE;
            desired |= DELETE;
        }
        if (flags & _O_SHORT_LIVED) {
            attributes &= ~FILE_ATTRIBUTE_NORMAL;
            attributes |= FILE_ATTRIBUTE_TEMPORARY;
        }
        if (flags & _O_SEQUENTIAL) attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
        if (flags & _O_RANDOM) attributes |= FILE_FLAG_RANDOM_ACCESS;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     (flags & _O_NOINHERIT) ? FALSE : TRUE};
        HANDLE handle =
            CreateFileW(wide.c_str(), desired,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        &security, creation, attributes, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return win_error(GetLastError());
        InvalidParameterScope parameters;
        const int fd = _open_osfhandle(
            reinterpret_cast<std::intptr_t>(handle),
            (flags & (_O_WRONLY | _O_RDWR | _O_APPEND | _O_NOINHERIT)) |
                _O_BINARY);
        if (fd < 0) {
            const int error = errno;
            CloseHandle(handle);
            errno = error;
        }
        return fd;
    } catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
#else
    return ::open(path, flags, mode);
#endif
}

int close(int fd) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _close(fd);
#else
    return ::close(fd);
#endif
}

IoResult read(int fd, void* buf, std::size_t len) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _read(fd, buf,
                 static_cast<unsigned int>(
                     std::min(len, static_cast<std::size_t>(INT_MAX))));
#else
    return ::read(fd, buf, len);
#endif
}

IoResult write(int fd, const void* buf, std::size_t len) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _write(fd, buf,
                  static_cast<unsigned int>(
                      std::min(len, static_cast<std::size_t>(INT_MAX))));
#else
    return ::write(fd, buf, len);
#endif
}

IoResult pread(int fd, void* buf, std::size_t len, FileOffset offset) noexcept {
#ifdef _WIN32
    return positional_io(fd, buf, len, offset, false);
#else
    return ::pread(fd, buf, len, offset);
#endif
}

IoResult pwrite(int fd, const void* buf, std::size_t len,
                FileOffset offset) noexcept {
#ifdef _WIN32
    return positional_io(fd, const_cast<void*>(buf), len, offset, true);
#else
    return ::pwrite(fd, buf, len, offset);
#endif
}

int fsync(int fd) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _commit(fd);
#else
    return ::fsync(fd);
#endif
}

int truncate(int fd, FileOffset length) noexcept {
#ifdef _WIN32
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    InvalidParameterScope parameters;
    const int error = _chsize_s(fd, length);
    if (error == 0) return 0;
    errno = error;
    return -1;
#else
    return ::ftruncate(fd, length);
#endif
}

FileOffset seek(int fd, FileOffset offset, int whence) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _lseeki64(fd, offset, whence);
#else
    return ::lseek(fd, offset, whence);
#endif
}

int stat_fd(int fd, FileStat* buf) noexcept {
#ifdef _WIN32
    InvalidParameterScope parameters;
    return _fstat64(fd, buf);
#else
    return ::fstat(fd, buf);
#endif
}

int iov_max_entries() noexcept {
#ifdef _WIN32
    return 1024;
#else
    static const int value = [] {
        const long n = ::sysconf(_SC_IOV_MAX);
        return n > 0 && n <= INT_MAX ? static_cast<int>(n) : 1024;
    }();
    return value;
#endif
}

std::size_t mapping_granularity() noexcept {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
#else
    const long page = ::sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<std::size_t>(page) : 4096;
#endif
}

void* map_readonly(int fd, FileOffset offset, std::size_t length) noexcept {
    if (offset < 0 || length == 0 ||
        static_cast<std::uint64_t>(offset) % mapping_granularity() != 0) {
        errno = EINVAL;
        return nullptr;
    }
#ifdef _WIN32
    InvalidParameterScope parameters;
    const auto raw = _get_osfhandle(fd);
    if (raw == -1 || raw == -2) {
        errno = EBADF;
        return nullptr;
    }
    ScopedHandle mapping(CreateFileMappingW(
        reinterpret_cast<HANDLE>(raw), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (mapping.get() == nullptr) {
        win_error(GetLastError());
        return nullptr;
    }
    void* address = MapViewOfFile(
        mapping.get(), FILE_MAP_READ,
        static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32),
        static_cast<DWORD>(offset), length);
    if (!address) win_error(GetLastError());
    // The view keeps the section alive after its mapping handle is closed.
    return address;
#else
    void* address = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, offset);
    return address == MAP_FAILED ? nullptr : address;
#endif
}

int unmap(void* address, std::size_t length) noexcept {
#ifdef _WIN32
    (void)length;
    return UnmapViewOfFile(address) ? 0 : win_error(GetLastError());
#else
    return ::munmap(address, length);
#endif
}

#ifdef _WIN32
namespace {

IoResult vector_io(int fd, const IoVec* iov, int count, FileOffset offset,
                   bool positional, bool writing) noexcept {
    if (count < 0 || count > iov_max_entries() || (positional && offset < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (count > 0 && iov == nullptr) {
        errno = EFAULT;
        return -1;
    }
    std::size_t total = 0;
    for (int i = 0; i < count; ++i) {
        if (iov[i].iov_len >
            static_cast<std::size_t>(std::numeric_limits<IoResult>::max()) -
                total) {
            errno = EINVAL;
            return -1;
        }
        if (iov[i].iov_len > 0 && iov[i].iov_base == nullptr) {
            errno = EFAULT;
            return -1;
        }
        total += iov[i].iov_len;
    }
    // Gather/scatter around one I/O call so vectors do not independently move
    // a shared descriptor or issue multiple writes to a pipe. A short result
    // is valid; callers such as writev_all advance and retry the remaining
    // data.
    const auto size = std::min(total, static_cast<std::size_t>(INT_MAX));
    try {
        std::vector<char> buffer(size);
        std::size_t copied = 0;
        if (writing) {
            for (int i = 0; i < count && copied < size; ++i) {
                const auto n = std::min(iov[i].iov_len, size - copied);
                if (n > 0)
                    std::memcpy(buffer.data() + copied, iov[i].iov_base, n);
                copied += n;
            }
        }
        // Give the CRT a valid address even for an empty vector.
        char empty = 0;
        void* data = size > 0 ? buffer.data() : &empty;
        IoResult result;
        if (positional) {
            result = positional_io(fd, data, size, offset, writing);
        } else {
            result = writing ? platform_io::write(fd, data, size)
                             : platform_io::read(fd, data, size);
        }
        if (!writing && result > 0) {
            copied = 0;
            for (int i = 0;
                 i < count && copied < static_cast<std::size_t>(result); ++i) {
                const auto n = std::min(
                    iov[i].iov_len, static_cast<std::size_t>(result) - copied);
                if (n > 0)
                    std::memcpy(iov[i].iov_base, buffer.data() + copied, n);
                copied += n;
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int socket_error(int error) noexcept {
    switch (error) {
        case WSAEINTR:
            errno = EINTR;
            break;
        case WSAEBADF:
            errno = EBADF;
            break;
        case WSAEACCES:
            errno = EACCES;
            break;
        case WSAEFAULT:
            errno = EFAULT;
            break;
        case WSAEINVAL:
            errno = EINVAL;
            break;
        case WSAEMFILE:
            errno = EMFILE;
            break;
        case WSAEWOULDBLOCK:
            errno = EAGAIN;
            break;
        case WSAEINPROGRESS:
            errno = EINPROGRESS;
            break;
        case WSAEALREADY:
            errno = EALREADY;
            break;
        case WSAENOTSOCK:
            errno = ENOTSOCK;
            break;
        case WSAEDESTADDRREQ:
            errno = EDESTADDRREQ;
            break;
        case WSAEMSGSIZE:
            errno = EMSGSIZE;
            break;
        case WSAEPROTOTYPE:
            errno = EPROTOTYPE;
            break;
        case WSAENOPROTOOPT:
            errno = ENOPROTOOPT;
            break;
        case WSAEPROTONOSUPPORT:
            errno = EPROTONOSUPPORT;
            break;
        case WSAEOPNOTSUPP:
            errno = EOPNOTSUPP;
            break;
        case WSAEAFNOSUPPORT:
            errno = EAFNOSUPPORT;
            break;
        case WSAEADDRINUSE:
            errno = EADDRINUSE;
            break;
        case WSAEADDRNOTAVAIL:
            errno = EADDRNOTAVAIL;
            break;
        case WSAENETDOWN:
            errno = ENETDOWN;
            break;
        case WSAENETUNREACH:
            errno = ENETUNREACH;
            break;
        case WSAENETRESET:
            errno = ENETRESET;
            break;
        case WSAECONNABORTED:
            errno = ECONNABORTED;
            break;
        case WSAECONNRESET:
            errno = ECONNRESET;
            break;
        case WSAENOBUFS:
            errno = ENOBUFS;
            break;
        case WSAEISCONN:
            errno = EISCONN;
            break;
        case WSAENOTCONN:
            errno = ENOTCONN;
            break;
        case WSAESHUTDOWN:
            errno = EPIPE;
            break;
        case WSAETIMEDOUT:
            errno = ETIMEDOUT;
            break;
        case WSAECONNREFUSED:
            errno = ECONNREFUSED;
            break;
        case WSAEHOSTUNREACH:
            errno = EHOSTUNREACH;
            break;
        case WSANOTINITIALISED:
            errno = ENETDOWN;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}

}  // namespace
#endif

IoResult readv(int fd, const IoVec* iov, int count) noexcept {
#ifdef _WIN32
    return vector_io(fd, iov, count, 0, false, false);
#else
    return ::readv(fd, iov, count);
#endif
}

IoResult writev(int fd, const IoVec* iov, int count) noexcept {
#ifdef _WIN32
    return vector_io(fd, iov, count, 0, false, true);
#else
    return ::writev(fd, iov, count);
#endif
}

IoResult preadv(int fd, const IoVec* iov, int count,
                FileOffset offset) noexcept {
#ifdef _WIN32
    return vector_io(fd, iov, count, offset, true, false);
#else
    return ::preadv(fd, iov, count, offset);
#endif
}

IoResult pwritev(int fd, const IoVec* iov, int count,
                 FileOffset offset) noexcept {
#ifdef _WIN32
    return vector_io(fd, iov, count, offset, true, true);
#else
    return ::pwritev(fd, iov, count, offset);
#endif
}

IoResult accept(SocketHandle socket, sockaddr* addr, SocketLength* addrlen,
                bool nonblocking) noexcept {
#ifdef __linux__
    if (nonblocking)
        return ::accept4(socket, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    const auto accepted = ::accept(socket, addr, addrlen);
#ifdef _WIN32
    if (accepted == INVALID_SOCKET) return socket_error(WSAGetLastError());
    // Match close-on-exec behavior without narrowing the SOCKET to a CRT fd.
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(accepted),
                              HANDLE_FLAG_INHERIT, 0)) {
        const auto error = GetLastError();
        ::closesocket(accepted);
        return win_error(error);
    }
    if (nonblocking) {
        u_long mode = 1;
        if (::ioctlsocket(accepted, FIONBIO, &mode) != 0) {
            const auto error = WSAGetLastError();
            ::closesocket(accepted);
            return socket_error(error);
        }
    }
#else
    if (accepted < 0) return -1;
    if (nonblocking) {
        const int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags < 0 || ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(accepted, F_SETFD, FD_CLOEXEC) < 0) {
            const int error = errno;
            ::close(accepted);
            errno = error;
            return -1;
        }
    }
#endif
    return static_cast<IoResult>(accepted);
}

IoResult recv(SocketHandle socket, void* buf, std::size_t len,
              int flags) noexcept {
#ifdef _WIN32
    const int result = ::recv(
        socket, static_cast<char*>(buf),
        static_cast<int>(std::min(len, static_cast<std::size_t>(INT_MAX))),
        flags);
    return result == SOCKET_ERROR ? socket_error(WSAGetLastError()) : result;
#else
    return ::recv(socket, buf, len, flags);
#endif
}

IoResult send(SocketHandle socket, const void* buf, std::size_t len,
              int flags) noexcept {
#ifdef _WIN32
    const int result = ::send(
        socket, static_cast<const char*>(buf),
        static_cast<int>(std::min(len, static_cast<std::size_t>(INT_MAX))),
        flags);
    return result == SOCKET_ERROR ? socket_error(WSAGetLastError()) : result;
#else
    return ::send(socket, buf, len, flags);
#endif
}

}  // namespace dftracer::utils::io::platform_io

#ifdef _WIN32
namespace dftracer::utils::io {

IoResult platform_sendfile(int out_fd, int in_fd, FileOffset offset,
                           std::size_t count) noexcept {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    // File descriptors (including CRT pipes) remain distinct from SOCKETs.
    // Reading positionally leaves the input descriptor's position untouched.
    char buffer[8192];
    IoResult total = 0;
    count = std::min(
        count, static_cast<std::size_t>(std::numeric_limits<IoResult>::max()));
    while (count > 0) {
        const auto chunk = std::min(count, sizeof(buffer));
        if (offset > std::numeric_limits<FileOffset>::max() -
                         static_cast<FileOffset>(chunk)) {
            errno = EOVERFLOW;
            return total > 0 ? total : -1;
        }
        const auto n = platform_io::pread(in_fd, buffer, chunk, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return total > 0 ? total : n;
        IoResult written = 0;
        while (written < n) {
            const auto w =
                platform_io::write(out_fd, buffer + written,
                                   static_cast<std::size_t>(n - written));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                if (w == 0) errno = EIO;
                return total > 0 ? total : -1;
            }
            written += w;
            total += w;
        }
        offset += n;
        count -= static_cast<std::size_t>(n);
    }
    return total;
}

}  // namespace dftracer::utils::io
#endif
