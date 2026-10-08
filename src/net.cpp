// SPDX-License-Identifier: MIT
#include "proxy/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <system_error>
#include <unistd.h>

namespace proxy {

std::optional<Address> resolve(std::string_view host_port) {
    std::string host, port;
    if (!host_port.empty() && host_port.front() == '[') {  // [::1]:9000
        size_t close = host_port.find(']');
        if (close == std::string_view::npos || close + 1 >= host_port.size() ||
            host_port[close + 1] != ':')
            return std::nullopt;
        host = host_port.substr(1, close - 1);
        port = host_port.substr(close + 2);
    } else {
        size_t colon = host_port.rfind(':');
        if (colon == std::string_view::npos) return std::nullopt;
        host = host_port.substr(0, colon);
        port = host_port.substr(colon + 1);
    }
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    if (host.empty()) host = "0.0.0.0";

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr)
        return std::nullopt;
    Address address;
    std::memcpy(&address.storage, found->ai_addr, found->ai_addrlen);
    address.length = found->ai_addrlen;
    address.text = std::string(host_port);
    freeaddrinfo(found);
    return address;
}

std::string to_text(const sockaddr_storage& addr) {
    char ip[INET6_ADDRSTRLEN] = "?";
    uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip);
        port = ntohs(v4->sin_port);
        return std::string(ip) + ":" + std::to_string(port);
    }
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof ip);
    port = ntohs(v6->sin6_port);
    return "[" + std::string(ip) + "]:" + std::to_string(port);
}

uint16_t local_port(int fd) {
    sockaddr_storage addr{};
    socklen_t length = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    if (addr.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
}

void tune(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

int listen_on(const Address& address, int backlog) {
    int fd = socket(address.storage.ss_family, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, address.get(), address.length) < 0 || listen(fd, backlog) < 0) {
        int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "listen on " + address.text);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

#if defined(__linux__)
namespace {
void no_delay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
}  // namespace
#endif

int accept_client(int listen_fd, sockaddr_storage& peer) {
    socklen_t length = sizeof peer;
    auto* addr = reinterpret_cast<sockaddr*>(&peer);
#if defined(__linux__)
    // Non-blocking and close-on-exec in the same call: two fewer syscalls per connection.
    int fd = accept4(listen_fd, addr, &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) no_delay(fd);
#else
    int fd = accept(listen_fd, addr, &length);
    if (fd >= 0) tune(fd);
#endif
    return fd;
}

int connect_to(const Address& address) {
#if defined(__linux__)
    int fd = socket(address.storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    no_delay(fd);
#else
    int fd = socket(address.storage.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tune(fd);
#endif
    if (connect(fd, address.get(), address.length) == 0 || errno == EINPROGRESS) return fd;
    int err = errno;
    ::close(fd);
    errno = err;
    return -1;
}

int connect_result(int fd) {
    int err = 0;
    socklen_t length = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) < 0) return errno;
    return err;
}

uint64_t hash_ip(const sockaddr_storage& addr) {
    const unsigned char* bytes;
    size_t n;
    if (addr.ss_family == AF_INET) {
        bytes = reinterpret_cast<const unsigned char*>(
            &reinterpret_cast<const sockaddr_in*>(&addr)->sin_addr);
        n = 4;
    } else {
        bytes = reinterpret_cast<const unsigned char*>(
            &reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_addr);
        n = 16;
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (size_t i = 0; i < n; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

long send_some(int fd, const char* data, size_t length) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, data, length, MSG_NOSIGNAL);
#else
    return ::send(fd, data, length, 0);  // SO_NOSIGPIPE is set by tune()
#endif
}

}  // namespace proxy
