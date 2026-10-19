#ifndef DFTRACER_UTILS_SERVER_SOCKET_H
#define DFTRACER_UTILS_SERVER_SOCKET_H

#include <dftracer/utils/core/io/types.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dftracer::utils::server {

inline constexpr io::SocketHandle INVALID_SOCKET_HANDLE =
    static_cast<io::SocketHandle>(-1);

inline int close_socket(io::SocketHandle socket) {
#ifdef _WIN32
    return ::closesocket(socket);
#else
    return ::close(socket);
#endif
}

inline int shutdown_socket(io::SocketHandle socket) {
#ifdef _WIN32
    return ::shutdown(socket, SD_BOTH);
#else
    return ::shutdown(socket, SHUT_RDWR);
#endif
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_SOCKET_H
