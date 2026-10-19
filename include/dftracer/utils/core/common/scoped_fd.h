#ifndef DFTRACER_UTILS_CORE_COMMON_SCOPED_FD_H
#define DFTRACER_UTILS_CORE_COMMON_SCOPED_FD_H

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace dftracer::utils {

struct ScopedFd {
    int value = -1;

    ScopedFd() = default;
    explicit ScopedFd(int fd) : value(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : value(other.value) {
        other.value = -1;
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset();
            value = other.value;
            other.value = -1;
        }
        return *this;
    }

    ~ScopedFd() { reset(); }

    void reset() {
        if (value >= 0) {
#ifdef _WIN32
            ::_close(value);
#else
            ::close(value);
#endif
            value = -1;
        }
    }

    int get() const { return value; }
};

}  // namespace dftracer::utils

#endif
