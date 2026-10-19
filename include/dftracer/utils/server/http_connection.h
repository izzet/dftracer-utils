#ifndef DFTRACER_UTILS_SERVER_HTTP_CONNECTION_H
#define DFTRACER_UTILS_SERVER_HTTP_CONNECTION_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/types.h>

#ifdef _WIN32
struct sockaddr_in;
#else
#include <netinet/in.h>
#endif

namespace dftracer::utils::server {

class Router;

/// Handle a single HTTP/1.1 connection: read requests, route them,
/// send responses. Supports keep-alive (HTTP/1.1 default).
coro::CoroTask<void> handle_connection(io::SocketHandle client_fd,
                                       struct sockaddr_in addr, Router& router);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_HTTP_CONNECTION_H
