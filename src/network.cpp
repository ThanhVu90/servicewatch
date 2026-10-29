#include "servicewatch.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace sw {
#ifdef _WIN32
using Native = SOCKET;
constexpr Native invalid = INVALID_SOCKET;
static int last_error() { return WSAGetLastError(); }
static bool pending(int code) {
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEINTR;
}
static bool interrupted(int code) { return code == WSAEINTR; }
#else
using Native = int;
constexpr Native invalid = -1;
static int last_error() { return errno; }
static bool pending(int code) {
    return code == EINPROGRESS || code == EWOULDBLOCK || code == EINTR;
}
static bool interrupted(int code) { return code == EINTR; }
#endif
class Socket {
    Native handle_;

  public:
    explicit Socket(int family) : handle_(::socket(family, SOCK_STREAM, IPPROTO_TCP)) {
        if (handle_ == invalid)
            throw std::runtime_error("cannot create socket");
    }
    ~Socket() {
#ifdef _WIN32
        closesocket(handle_);
#else
        close(handle_);
#endif
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Native get() const { return handle_; }
};
Network::Network() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        throw std::runtime_error("Winsock initialization failed");
#endif
}
Network::~Network() {
#ifdef _WIN32
    WSACleanup();
#endif
}
bool numeric_address(const std::string &s) {
    if (s.empty() || s.size() > 45 || s.find('\0') != std::string::npos)
        return false;
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, s.c_str(), &v4) == 1 || inet_pton(AF_INET6, s.c_str(), &v6) == 1;
}
Observation tcp_probe(const Endpoint &e, std::chrono::milliseconds timeout) {
    if (!numeric_address(e.address) || e.port == 0 || timeout.count() < 1 ||
        timeout.count() > 30000)
        throw std::invalid_argument("invalid probe arguments");
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now(), deadline = started + timeout;
    auto result = [&](bool ok, const char *why) {
        return Observation{
            ok,
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(),
            why};
    };
    const int family = e.address.find(':') == std::string::npos ? AF_INET : AF_INET6;
    sockaddr_storage storage{};
    int length;
    if (family == AF_INET) {
        auto *a = reinterpret_cast<sockaddr_in *>(&storage);
        a->sin_family = AF_INET;
        a->sin_port = htons(e.port);
        inet_pton(AF_INET, e.address.c_str(), &a->sin_addr);
        length = sizeof(sockaddr_in);
    } else {
        auto *a = reinterpret_cast<sockaddr_in6 *>(&storage);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(e.port);
        inet_pton(AF_INET6, e.address.c_str(), &a->sin6_addr);
        length = sizeof(sockaddr_in6);
    }
    Socket socket(family);
    const Native fd = socket.get();
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0)
        throw std::runtime_error("cannot configure socket");
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("cannot configure socket");
#endif
    if (::connect(fd, reinterpret_cast<sockaddr *>(&storage), static_cast<int>(length)) == 0)
        return result(true, "connected");
    if (!pending(last_error()))
        return result(false, "connect_error");
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0)
            return result(false, "timeout");
        int ready;
#ifdef _WIN32
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(fd, &writes);
        FD_SET(fd, &errors);
        timeval wait{};
        wait.tv_sec = static_cast<long>(remaining / 1000);
        wait.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        ready = select(0, nullptr, &writes, &errors, &wait);
#else
        pollfd entry{fd, POLLOUT, 0};
        ready = poll(&entry, 1, static_cast<int>(remaining));
#endif
        if (ready == 0)
            return result(false, "timeout");
        if (ready < 0) {
            if (interrupted(last_error()))
                continue;
            throw std::runtime_error("socket wait failed");
        }
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) != 0)
            throw std::runtime_error("socket status failed");
#else
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
            throw std::runtime_error("socket status failed");
#endif
        if (error != 0)
            return result(false, "connect_error");
        return result(true, "connected");
    }
}
} // namespace sw
