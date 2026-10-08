// SPDX-License-Identifier: MIT
#pragma once

// Socket plumbing: addresses, listening, non-blocking connects.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace proxy {

struct Address {
    sockaddr_storage storage{};
    socklen_t length = 0;
    std::string text;  // as given, for logs: "127.0.0.1:9001"

    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&storage); }
};

// "host:port", "[v6]:port" or ":port" (every interface). Hostnames are
// resolved once, here. Returns nothing if it cannot be parsed or resolved.
std::optional<Address> resolve(std::string_view host_port);

// "ip:port" of a peer, for logs.
std::string to_text(const sockaddr_storage& addr);

// The port a socket is bound to (for tests that listen on port 0).
uint16_t local_port(int fd);

// A non-blocking listening socket; throws on failure.
int listen_on(const Address& address, int backlog = 4096);

// Accepts one connection, already tuned; -1 with errno set if none was waiting.
int accept_client(int listen_fd, sockaddr_storage& peer);

// Starts a non-blocking connect. Returns the socket, or -1 with errno set if
// it failed at once (EINPROGRESS is not a failure: the result comes later).
int connect_to(const Address& address);

// The outcome of a non-blocking connect once the socket is writable: 0 or an errno.
int connect_result(int fd);

// Non-blocking, close-on-exec, no Nagle, and no SIGPIPE where the platform needs it per socket.
void tune(int fd);

// A stable hash of a peer's IP address (not its port), for source-hash balancing.
uint64_t hash_ip(const sockaddr_storage& addr);

// send() without SIGPIPE on a closed peer.
long send_some(int fd, const char* data, size_t length);

}  // namespace proxy
