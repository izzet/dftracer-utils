#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/cancel.h>
#include <dftracer/utils/server/http_connection.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/tcp_listener.h>
#include <doctest/doctest.h>

// Keep native socket headers after the public project headers.
#include <dftracer/utils/server/socket.h>

#include <array>
#include <chrono>
#include <string>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

namespace {
struct SocketOwner {
    io::SocketHandle value = INVALID_SOCKET_HANDLE;
    ~SocketOwner() {
        if (value != INVALID_SOCKET_HANDLE) close_socket(value);
    }
};

void connect_client(TcpListener& listener, SocketOwner& client,
                    SocketOwner& accepted) {
    sockaddr_in address{};
    io::SocketLength size = sizeof(address);
    REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address),
                          &size) == 0);
    client.value = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client.value != INVALID_SOCKET_HANDLE);
    REQUIRE(::connect(client.value, reinterpret_cast<sockaddr*>(&address),
                      size) == 0);
    const auto result =
        io::platform_io::accept(listener.fd(), nullptr, nullptr);
    REQUIRE(result >= 0);
    accepted.value = static_cast<io::SocketHandle>(result);
#ifdef _WIN32
    const DWORD timeout = 5000;
    ::setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{5, 0};
    ::setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout));
#endif
}

HttpResponse::StreamGenerator response_chunks() {
    const std::array<std::string_view, 2> values{"alpha", "beta"};
    co_yield HttpResponse::StreamChunk{values};
}
}  // namespace

TEST_CASE("TCP sockets and disconnect cancellation retain native handles") {
    TcpListener listener("127.0.0.1", 0);
    REQUIRE(listener.start());
    SocketOwner client, accepted;
    connect_client(listener, client, accepted);
    auto token = CancelRegistry::instance().create("", accepted.value);
    CHECK_FALSE(token.cancelled());
    REQUIRE(io::platform_io::send(client.value, "x", 1, 0) == 1);
    for (int probe = 0; probe < 17; ++probe) CHECK_FALSE(token.cancelled());
    char byte;
    REQUIRE(io::platform_io::recv(accepted.value, &byte, 1, 0) == 1);
    REQUIRE(close_socket(client.value) == 0);
    client.value = INVALID_SOCKET_HANDLE;
    bool cancelled = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!cancelled && std::chrono::steady_clock::now() < deadline) {
        cancelled = token.cancelled();
        if (!cancelled)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(cancelled);
    listener.stop();
    CHECK(listener.fd() == INVALID_SOCKET_HANDLE);
}

TEST_CASE("HTTP responses and chunked streams use native socket I/O") {
    for (const bool streaming : {false, true}) {
        TcpListener listener("127.0.0.1", 0);
        REQUIRE(listener.start());
        SocketOwner client, accepted;
        connect_client(listener, client, accepted);
        Router router;
        router.get(
            "/",
            [streaming](const HttpRequest&,
                        const QueryParams&) -> coro::CoroTask<HttpResponse> {
                if (streaming)
                    co_return HttpResponse::streaming(
                        std::make_unique<HttpResponse::StreamGenerator>(
                            response_chunks()));
                co_return HttpResponse::ok("hello");
            });
        Runtime runtime(2);
        auto serve = [&]() -> coro::CoroTask<void> {
            co_await handle_connection(accepted.value, sockaddr_in{}, router);
            shutdown_socket(accepted.value);
        };
        auto done = runtime.submit(serve(), "platform-http");
        const std::string request =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        REQUIRE(io::platform_io::send(client.value, request.data(),
                                      request.size(), 0) ==
                static_cast<io::IoResult>(request.size()));
        std::string response;
        std::array<char, 256> buffer;
        for (;;) {
            const auto count = io::platform_io::recv(
                client.value, buffer.data(), buffer.size(), 0);
            REQUIRE(count >= 0);
            if (count == 0) break;
            response.append(buffer.data(), static_cast<std::size_t>(count));
        }
        done.get();
        runtime.shutdown();
        CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
        if (streaming) {
            CHECK(response.find("b\r\nalpha\nbeta\n\r\n0\r\n\r\n") !=
                  std::string::npos);
        } else {
            CHECK(response.ends_with("hello"));
        }
    }
}

TEST_CASE("stopping the listener unblocks its asynchronous accept loop") {
    TcpListener listener("127.0.0.1", 0);
    REQUIRE(listener.start());
    sockaddr_in address{};
    io::SocketLength size = sizeof(address);
    REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address),
                          &size) == 0);
    Runtime runtime(2);
    auto task = run_coro_scope(
        runtime.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            co_await listener.accept_loop(
                scope,
                [](io::SocketHandle fd, sockaddr_in) -> coro::CoroTask<void> {
                    co_await io::send(fd, "ready", 5, 0);
                });
        });
    auto done = runtime.submit(std::move(task), "platform-accept");
    SocketOwner client;
    client.value = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client.value != INVALID_SOCKET_HANDLE);
    REQUIRE(::connect(client.value, reinterpret_cast<sockaddr*>(&address),
                      size) == 0);
    std::array<char, 5> response{};
    REQUIRE(io::platform_io::recv(client.value, response.data(),
                                  response.size(), 0) == 5);
    CHECK(std::string(response.data(), response.size()) == "ready");
    listener.stop();
    done.get();
    runtime.shutdown();
    CHECK(listener.fd() == INVALID_SOCKET_HANDLE);
}
