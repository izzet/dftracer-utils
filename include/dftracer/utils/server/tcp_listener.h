#ifndef DFTRACER_UTILS_SERVER_TCP_LISTENER_H
#define DFTRACER_UTILS_SERVER_TCP_LISTENER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/types.h>

#ifdef _WIN32
struct sockaddr_in;
#else
#include <netinet/in.h>
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

namespace dftracer::utils {
class CoroScope;
}

namespace dftracer::utils::server {

/// TCP listener that accepts connections and spawns per-connection
/// coroutine handlers using structured concurrency.
class TcpListener {
   public:
    TcpListener(const std::string& addr, uint16_t port);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Bind and listen. Returns true on success.
    bool start(int backlog = 128);

    /// Accept loop: yields new client fds via coroutine.
    /// Runs until stop() is called or the listen socket is closed.
    /// Spawns handler(client_fd, addr) for each accepted connection.
    using ConnectionHandler = std::function<coro::CoroTask<void>(
        io::SocketHandle client_fd, struct sockaddr_in addr)>;
    coro::CoroTask<void> accept_loop(CoroScope& scope,
                                     ConnectionHandler handler);

    /// Request graceful shutdown. The accept loop will break on the
    /// next iteration. In-flight handlers drain via CoroScope.
    void stop();

    io::SocketHandle fd() const { return listen_fd_; }
    uint16_t port() const { return port_; }

   private:
    /// Track live client fds so shutdown can interrupt handlers parked in recv
    /// on keep-alive connections (otherwise the accept loop breaks but the
    /// spawned handlers never return and the scope never joins).
    void track_active(io::SocketHandle fd);
    void untrack_active(io::SocketHandle fd);
    void shutdown_active();

    std::string bind_addr_;
    uint16_t port_;
    std::atomic<io::SocketHandle> listen_fd_{static_cast<io::SocketHandle>(-1)};
    std::atomic<bool> stopped_{false};
    std::mutex active_mu_;
    std::unordered_set<io::SocketHandle> active_fds_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TCP_LISTENER_H
