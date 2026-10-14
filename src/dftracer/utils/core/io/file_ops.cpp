#include <dftracer/utils/core/io/file_ops.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <limits>

#ifdef _WIN32
#include <io.h>
#include <stdlib.h>

#include <bit>
#include <new>
#include <string>
#else
#include <unistd.h>
#endif

namespace dftracer::utils::io::file_ops {

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

}  // namespace dftracer::utils::io::file_ops
