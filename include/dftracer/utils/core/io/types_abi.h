#ifndef DFTRACER_UTILS_CORE_IO_TYPES_ABI_H
#define DFTRACER_UTILS_CORE_IO_TYPES_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
struct sockaddr;
typedef uintptr_t dftu_socket_t;
typedef intptr_t dftu_socket_result_t;
typedef int dftu_socklen_t;
typedef struct dftu_iovec {
    void* iov_base;
    size_t iov_len;
} dftu_iovec;
#else
#include <sys/socket.h>
#include <sys/uio.h>
typedef int dftu_socket_t;
typedef int dftu_socket_result_t;
typedef socklen_t dftu_socklen_t;
typedef struct iovec dftu_iovec;
#endif

#endif /* DFTRACER_UTILS_CORE_IO_TYPES_ABI_H */
