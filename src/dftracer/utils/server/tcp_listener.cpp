#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/io/types.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/socket.h>
#include <dftracer/utils/server/tcp_listener.h>

#include <cerrno>
#include <cstring>
#include <exception>

namespace dftracer::utils::server {

TcpListener::TcpListener(const std::string& addr, uint16_t port)
    : bind_addr_(addr), port_(port) {}

TcpListener::~TcpListener() { stop(); }

bool TcpListener::start(int backlog) {
#ifdef _WIN32
    static const int startup = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (startup != 0) return false;
#endif
#ifdef SOCK_CLOEXEC
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (listen_fd_ == INVALID_SOCKET_HANDLE) {
        DFTRACER_UTILS_LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }
#ifdef _WIN32
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(listen_fd_.load()),
                              HANDLE_FLAG_INHERIT, 0)) {
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_HANDLE;
        return false;
    }
#elif !defined(SOCK_CLOEXEC)
    ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
#endif

    int opt = 1;
#ifdef _WIN32
    // Windows SO_REUSEADDR can let another listener hijack the same port.
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
            DFTRACER_UTILS_LOG_ERROR("Invalid bind address: %s",
                                     bind_addr_.c_str());
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_HANDLE;
            return false;
        }
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        DFTRACER_UTILS_LOG_ERROR("bind(%s:%u) failed: %s", bind_addr_.c_str(),
                                 port_, std::strerror(errno));
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_HANDLE;
        return false;
    }

    if (::listen(listen_fd_, backlog) < 0) {
        DFTRACER_UTILS_LOG_ERROR("listen() failed: %s", std::strerror(errno));
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_HANDLE;
        return false;
    }

    return true;
}

coro::CoroTask<void> TcpListener::accept_loop(CoroScope& scope,
                                              ConnectionHandler handler) {
    while (!stopped_.load(std::memory_order_acquire) &&
           !g_shutdown_requested.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr{};
        io::SocketLength addrlen = sizeof(client_addr);

        io::IoResult client_fd = co_await io::accept(
            listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
            &addrlen);

        if (client_fd < 0) {
            if (client_fd == -EINTR || client_fd == -EAGAIN) continue;
            // Listener stopped or fatal error
            break;
        }

        // Spawn a coroutine per connection (structured concurrency).
        // Capture by value: fd and addr are small.
        auto* handler_ptr = &handler;
        io::SocketHandle fd = static_cast<io::SocketHandle>(client_fd);

        // Ensure the client socket is blocking so recv/send work
        // properly in the IO thread pool.
#ifndef _WIN32
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif

        struct sockaddr_in addr_copy = client_addr;
        track_active(fd);
        scope.spawn([this, fd, addr_copy, handler_ptr](
                        CoroScope& /*child*/) -> coro::CoroTask<void> {
            // Close sockets with the native API; they are not CRT descriptors.
            std::exception_ptr err;
            try {
                co_await (*handler_ptr)(fd, addr_copy);
            } catch (...) {
                err = std::current_exception();
            }
            untrack_active(fd);
            close_socket(fd);
            if (err) std::rethrow_exception(err);
        });
    }

    // Shutdown requested: unblock any handler parked in recv on a keep-alive
    // connection so the spawned coroutines return and the scope can join.
    shutdown_active();

    // The signal handler may have already closed listen_fd_ via
    // g_listen_fd.  Mark it as -1 to prevent a double-close in the
    // destructor or stop().
    if (g_shutdown_requested.load(std::memory_order_acquire) &&
        g_listen_fd.load(std::memory_order_acquire) == INVALID_SOCKET_HANDLE) {
        listen_fd_ = INVALID_SOCKET_HANDLE;
    }
}

void TcpListener::track_active(io::SocketHandle fd) {
    std::lock_guard<std::mutex> lock(active_mu_);
    active_fds_.insert(fd);
}

void TcpListener::untrack_active(io::SocketHandle fd) {
    std::lock_guard<std::mutex> lock(active_mu_);
    active_fds_.erase(fd);
}

void TcpListener::shutdown_active() {
    std::lock_guard<std::mutex> lock(active_mu_);
    for (io::SocketHandle fd : active_fds_) {
        // Wakes a blocked recv/send in the handler; the handler then closes fd.
        shutdown_socket(fd);
    }
}

void TcpListener::stop() {
    stopped_.store(true, std::memory_order_release);
    // Close the listen socket to unblock any pending accept.
    const auto fd = listen_fd_.exchange(INVALID_SOCKET_HANDLE);
    if (fd != INVALID_SOCKET_HANDLE) {
        auto registered = fd;
        g_listen_fd.compare_exchange_strong(registered, INVALID_SOCKET_HANDLE);
        shutdown_socket(fd);
        close_socket(fd);
    }
}

}  // namespace dftracer::utils::server
