#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/socket.h>

#include <csignal>
#include <cstring>

namespace dftracer::utils::server {

std::atomic<bool> g_shutdown_requested{false};
std::atomic<io::SocketHandle> g_listen_fd{INVALID_SOCKET_HANDLE};

static void signal_handler(int /*sig*/) {
    g_shutdown_requested.store(true, std::memory_order_release);
    const auto fd =
        g_listen_fd.exchange(INVALID_SOCKET_HANDLE, std::memory_order_acq_rel);
    if (fd != INVALID_SOCKET_HANDLE) {
        // shutdown() interrupts a blocked accept() on Linux.
        // close() interrupts a blocked accept() on macOS.
        // Both are async-signal-safe.  Use both for portability.
        shutdown_socket(fd);
        close_socket(fd);
    }
}

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
        event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
        return FALSE;
    signal_handler(0);
    return TRUE;
}
#endif

void install_signal_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
    std::signal(SIGTERM, signal_handler);
#else
    // Use sigaction instead of std::signal so the handler is NOT
    // reset to SIG_DFL after the first invocation (SA_RESETHAND is
    // not set) and SA_RESTART is not set so that blocked syscalls
    // like accept() return EINTR.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — accept() must return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGPIPE so broken client connections don't kill the server.
    // The send() call will return EPIPE instead.
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace dftracer::utils::server