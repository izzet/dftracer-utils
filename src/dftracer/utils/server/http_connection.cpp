#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/io/types.h>
#include <dftracer/utils/server/http_connection.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/socket.h>

#include <cstring>
#include <vector>

#ifdef __linux__
#include <malloc.h>  // malloc_trim
#endif

namespace dftracer::utils::server {

namespace {
coro::CoroTask<io::IoResult> send_all(io::SocketHandle fd, const char* data,
                                      std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto result = co_await io::send(fd, data + sent, size - sent, 0);
        if (result == -EINTR) continue;
        if (result <= 0) co_return result < 0 ? result : -EPIPE;
        sent += static_cast<std::size_t>(result);
    }
    co_return static_cast<io::IoResult>(sent);
}
}  // namespace

coro::CoroTask<void> handle_connection(io::SocketHandle client_fd,
                                       struct sockaddr_in /*addr*/,
                                       Router& router) {
    // 8 KiB receive buffer. For HTTP/1.1 GET requests this is plenty.
    // Requests larger than this are rejected as "too large".
    constexpr std::size_t BUF_SIZE = 8192;
    char buf[BUF_SIZE];
    std::size_t buf_used = 0;

    while (true) {
        // Read data from socket.
        io::IoResult n = co_await io::recv(client_fd, buf + buf_used,
                                           BUF_SIZE - buf_used, 0);
        if (n <= 0) break;  // Connection closed or error
        buf_used += static_cast<std::size_t>(n);

        // Try to parse a complete request.
        HttpRequest req;
        int parsed = req.parse(buf, buf_used);
        if (parsed == -2) {
            // Incomplete — need more data.
            if (buf_used >= BUF_SIZE) {
                // Buffer full but still no complete request.
                auto resp = HttpResponse::bad_request("Request too large");
                auto out = resp.serialize();
                co_await send_all(client_fd, out.data(), out.size());
                break;
            }
            continue;
        }
        if (parsed < 0) {
            auto resp = HttpResponse::bad_request("Malformed HTTP request");
            auto out = resp.serialize();
            co_await send_all(client_fd, out.data(), out.size());
            break;
        }

        // Route and handle. Register a cancellation token keyed by the
        // client-supplied request id so a separate cancel request can reach it.
        std::string req_id(req.header("x-request-id"));
        CancelToken token =
            CancelRegistry::instance().create(req_id, client_fd);
        req.cancel_token = token;

        // Remove the registration on every exit path, including a throw from
        // the streaming branch below, so a client-controlled req_id cannot leak
        // a CancelState for the process lifetime.
        struct CancelRegistrationGuard {
            const std::string& id;
            ~CancelRegistrationGuard() {
                CancelRegistry::instance().remove(id);
            }
        } cancel_guard{req_id};

        HttpResponse resp;
        try {
            resp = co_await router.handle(req);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Handler exception: %s", e.what());
            resp = HttpResponse::internal_error(e.what());
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR("Handler threw unknown exception");
            resp = HttpResponse::internal_error("Internal server error");
        }
        if (resp.is_streaming()) {
            auto hdrs = resp.serialize_headers();
            auto hdr_rc =
                co_await send_all(client_fd, hdrs.data(), hdrs.size());
            if (hdr_rc < 0) goto stream_done;

            {
                static constexpr char newline_ch = '\n';
                static constexpr char crlf[] = "\r\n";
                char hex[24];
                std::vector<io::IoVec> iovs;

                while (auto chunk = co_await resp.stream->next()) {
                    if (chunk->views.empty()) continue;

                    std::size_t payload_size = 0;
                    for (const auto& sv : chunk->views) {
                        payload_size += sv.size() + 1;
                    }

                    int hex_len = std::snprintf(hex, sizeof(hex), "%zx\r\n",
                                                payload_size);

                    iovs.clear();
                    iovs.reserve(chunk->views.size() * 2 + 2);
                    iovs.push_back({hex, static_cast<std::size_t>(hex_len)});
                    for (const auto& sv : chunk->views) {
                        iovs.push_back(
                            {const_cast<char*>(sv.data()), sv.size()});
                        iovs.push_back({const_cast<char*>(&newline_ch), 1});
                    }
                    iovs.push_back({const_cast<char*>(crlf), 2});

#ifdef _WIN32
                    // Winsock handles cannot be passed to CRT writev.
                    io::IoResult rc = 0;
                    for (const auto& iov : iovs) {
                        rc = co_await send_all(
                            client_fd, static_cast<const char*>(iov.iov_base),
                            iov.iov_len);
                        if (rc < 0) break;
                    }
#else
                    auto rc = co_await io::writev_all(
                        client_fd, iovs.data(), static_cast<int>(iovs.size()));
#endif
                    if (rc < 0) {
                        DFTRACER_UTILS_LOG_ERROR(
                            "Streaming write failed; closing connection");
                        token.cancel();
                        goto stream_done;
                    }
                }
            }
            co_await send_all(client_fd, "0\r\n\r\n", 5);
        stream_done:;
        } else {
            auto out = resp.serialize();
            co_await send_all(client_fd, out.data(), out.size());
        }

        // Consume parsed bytes; shift any remaining data.
        auto consumed = static_cast<std::size_t>(parsed);
        if (consumed < buf_used) {
            std::memmove(buf, buf + consumed, buf_used - consumed);
            buf_used -= consumed;
        } else {
            buf_used = 0;
        }

        // Check Connection: close or HTTP/1.0 (no keep-alive).
        if (req.minor_version == 0 || req.has_header("connection", "close")) {
            break;
        }
    }

    // Note: the caller (tcp_listener) is responsible for closing
    // client_fd with the native socket close after this coroutine returns.

#ifdef __linux__
    // Return freed heap pages to the OS.  Each request may decompress
    // large gzip buffers and build sizeable JSON responses; without
    // this call glibc's ptmalloc2 keeps those arenas mapped, causing
    // RSS to grow monotonically.
    ::malloc_trim(0);
#endif
}

}  // namespace dftracer::utils::server