// SPDX-License-Identifier: MIT
#pragma once

// Per-connection telemetry: one JSON line when a connection closes, saying
// where it came from, where it went, how long the connect took, how many bytes
// and system calls it cost and why it ended.

#include <cstdint>
#include <string>
#include <string_view>

namespace proxy {

enum class CloseReason : uint8_t {
    kDone,            // both sides finished cleanly
    kClientReset,     // the client's socket failed
    kBackendReset,    // the backend's socket failed after connecting
    kConnectFailed,   // every attempt to connect was refused or unreachable
    kConnectTimeout,  // the last attempt to connect timed out
    kNoBackend,       // the pool had no backend to try
    kIdleTimeout,     // nothing moved for too long
    kShutdown,        // still open when the proxy's drain period ended
};

const char* name(CloseReason reason);

struct ConnectionRecord {
    int64_t start_unix_ms = 0;  // wall clock at accept
    int worker = 0;
    std::string_view route;    // the listen address
    std::string_view client;   // ip:port
    std::string_view backend;  // host:port of the last backend tried, or empty
    int attempts = 0;
    int64_t connect_us = -1;  // -1 if it never connected
    int64_t duration_us = 0;
    uint64_t bytes_up = 0;    // client to backend
    uint64_t bytes_down = 0;  // backend to client
    uint32_t reads = 0;       // system calls, both sockets
    uint32_t writes = 0;
    CloseReason reason = CloseReason::kDone;
};

// Appends one line, ending in '\n'.
void append_json(std::string& out, const ConnectionRecord& record);

}  // namespace proxy
