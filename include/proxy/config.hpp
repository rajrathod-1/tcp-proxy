// SPDX-License-Identifier: MIT
#pragma once

// The configuration file: one directive per line, `#` starts a comment.
//
//   workers 4                       # 0 or absent: one per CPU
//   pool web least-connections 10.0.0.1:8080 10.0.0.2:8080
//   listen 0.0.0.0:80 web           # routing: each listener feeds one pool
//
// See proxy.conf.example for every directive.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "proxy/balancer.hpp"

namespace proxy {

struct PoolConfig {
    std::string name;
    Strategy strategy = Strategy::kRoundRobin;
    std::vector<std::string> backends;  // host:port
};

struct RouteConfig {
    std::string listen;  // host:port
    std::string pool;
};

struct Config {
    int workers = 0;
    size_t buffer_bytes = 16 * 1024;  // per direction, per connection
    int connect_timeout_ms = 3000;
    int idle_timeout_ms = 300'000;
    int drain_ms = 5000;   // on shutdown, how long open connections get to finish
    int max_attempts = 3;  // connects per client, each to a different backend
    HealthPolicy health;
    std::string log;  // per-connection telemetry: "" off, "-" stdout, else a file
    std::vector<PoolConfig> pools;
    std::vector<RouteConfig> routes;
};

// Throws std::runtime_error("line 7: ...") on the first problem.
Config parse_config(std::string_view text);

}  // namespace proxy
