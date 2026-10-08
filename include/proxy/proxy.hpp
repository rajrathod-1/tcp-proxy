// SPDX-License-Identifier: MIT
#pragma once

// The proxy: N worker threads, each running its own event loop over its own
// connections. Every listening socket is shared by all workers; the kernel
// wakes one of them per new connection. A connection is then owned by the
// worker that accepted it for its whole life, so no locks are taken on the
// data path. The only shared state is the balancer for each pool, which is
// atomic, and the totals below.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "proxy/balancer.hpp"
#include "proxy/config.hpp"
#include "proxy/net.hpp"

namespace proxy {

struct Pool {
    std::string name;
    std::vector<Address> backends;
    std::unique_ptr<Balancer> balancer;
};

struct Route {
    Address listen;
    int listen_fd = -1;
    Pool* pool = nullptr;
};

struct Totals {
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> closed{0};
    std::atomic<uint64_t> failed_connects{0};  // attempts refused, unreachable or timed out
    std::atomic<uint64_t> retries{0};          // attempts after a client's first
    std::atomic<uint64_t> failed_clients{0};   // clients closed without ever reaching a backend
    std::atomic<uint64_t> bytes_up{0};
    std::atomic<uint64_t> bytes_down{0};
};

class Proxy {
public:
    // Resolves every address and binds every listener; throws if any fails.
    explicit Proxy(Config config);
    ~Proxy();
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    // Runs the workers. Returns once stop() has been called and they have drained.
    void run();

    // Stop accepting; open connections get the drain period to finish. Safe from
    // any thread and from a signal handler.
    void stop() { stopping_.store(true, std::memory_order_relaxed); }
    bool stopping() const { return stopping_.load(std::memory_order_relaxed); }

    int workers() const;
    uint16_t port(size_t route = 0) const;
    const Pool& pool(size_t i) const { return *pools_[i]; }
    const Totals& totals() const { return totals_; }

private:
    friend class Worker;

    Config config_;
    std::vector<std::unique_ptr<Pool>> pools_;
    std::vector<Route> routes_;
    int log_fd_ = -1;
    bool own_log_fd_ = false;
    std::atomic<bool> stopping_{false};
    Totals totals_;
};

}  // namespace proxy
