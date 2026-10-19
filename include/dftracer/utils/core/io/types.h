#ifndef DFTRACER_UTILS_CORE_IO_TYPES_H
#define DFTRACER_UTILS_CORE_IO_TYPES_H

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <sys/stat.h>
// Do not expose Windows SDK macros (IN, ERROR, etc.) through public I/O
// headers. SOCKET is UINT_PTR; native declarations stay in platform sources.
struct sockaddr;
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#endif

namespace dftracer::utils::io {

#ifdef _WIN32
using IoResult = std::intptr_t;
using FileOffset = std::int64_t;
using FileMode = int;
using FileStat = struct ::_stat64;
using SocketHandle = std::uintptr_t;
using SocketLength = int;

struct IoVec {
    void* iov_base;
    std::size_t iov_len;
};
#else
using IoResult = ssize_t;
using FileOffset = off_t;
using FileMode = mode_t;
using FileStat = struct ::stat;
using SocketHandle = int;
using SocketLength = socklen_t;
using IoVec = struct ::iovec;
#endif

static_assert(sizeof(FileOffset) >= 8, "File offsets must support large files");
static_assert(sizeof(IoResult) >= sizeof(SocketHandle),
              "An I/O result must hold a socket handle without truncation");

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_TYPES_H
