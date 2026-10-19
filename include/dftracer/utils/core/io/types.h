#ifndef DFTRACER_UTILS_CORE_IO_TYPES_H
#define DFTRACER_UTILS_CORE_IO_TYPES_H

#include <dftracer/utils/core/io/types_abi.h>

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
#else
using IoResult = ssize_t;
using FileOffset = off_t;
using FileMode = mode_t;
using FileStat = struct ::stat;
#endif

using SocketHandle = dftu_socket_t;
using SocketLength = dftu_socklen_t;
using IoVec = dftu_iovec;

static_assert(sizeof(FileOffset) >= 8, "File offsets must support large files");
static_assert(sizeof(IoResult) >= sizeof(SocketHandle),
              "An I/O result must hold a socket handle without truncation");

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_TYPES_H
