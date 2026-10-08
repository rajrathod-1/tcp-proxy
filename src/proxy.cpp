// SPDX-License-Identifier: MIT
#include "proxy/proxy.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "proxy/buffer.hpp"
#include "proxy/poller.hpp"
#include "proxy/telemetry.hpp"

namespace proxy {

namespace {

using Clock = std::chrono::steady_clock;

int64_t ms_since_epoch(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

// What the poller hands back: which kind of socket became ready, and its owner.
struct Tag {
    enum Kind : uint8_t { kListener, kClient, kUpstream } kind;
    void* owner;
};

struct Connection {
    Connection(Route& r, int fd, size_t buffer_bytes, const sockaddr_storage& peer, Clock::time_point now)
        : route(&r),
          client_fd(fd),
          up(buffer_bytes),
          down(buffer_bytes),
          key(hash_ip(peer)),
          accepted_at(now),
          last_active_ms(ms_since_epoch(now)),
          start_unix_ms(unix_ms()),
          client_text(to_text(peer)) {}

    Tag client_tag{Tag::kClient, this};
    Tag upstream_tag{Tag::kUpstream, this};

    Route* route;
    int client_fd;
    int upstream_fd = -1;
    Buffer up;    // client to backend
    Buffer down;  // backend to client
    uint8_t client_interest = kNone;
    uint8_t upstream_interest = kNone;

    bool connecting = false;
    bool client_eof = false;     // the client has finished sending
    bool upstream_eof = false;   // the backend has finished sending
    bool client_shut = false;    // we have passed the backend's EOF on to the client
    bool upstream_shut = false;  // we have passed the client's EOF on to the backend
    bool timed_out = false;      // the last connect attempt timed out
    bool closed = false;

    int backend = Balancer::kNone;  // counted against in the balancer until released
    int last_backend = Balancer::kNone;
    int attempts = 0;
    uint64_t tried = 0;
    uint64_t key;

    Clock::time_point accepted_at;
    int64_t connect_started_ms = 0;
    int64_t last_active_ms;
    int64_t connect_us = -1;
    int64_t start_unix_ms;
    std::string client_text;
    uint64_t bytes_up = 0;
    uint64_t bytes_down = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
    size_t index = 0;  // position in the worker's list of live connections
};

}  // namespace

class Worker {
public:
    Worker(Proxy& proxy, int id) : proxy_(proxy), config_(proxy.config_), id_(id) {
        listener_tags_.reserve(proxy_.routes_.size());
        for (auto& route : proxy_.routes_) listener_tags_.push_back({Tag::kListener, &route});
    }

    void run() {
        listen(true);
        Ready ready[256];
        int64_t last_sweep = 0;
        int64_t drain_deadline = -1;
        for (;;) {
            int n = poller_.wait(ready, 256, 100);
            now_ = Clock::now();
            now_ms_ = ms_since_epoch(now_);
            for (int i = 0; i < n; ++i) {
                auto* tag = static_cast<Tag*>(ready[i].tag);
                if (tag->kind == Tag::kListener) {
                    accept_from(*static_cast<Route*>(tag->owner));
                    continue;
                }
                auto* c = static_cast<Connection*>(tag->owner);
                if (!c->closed) on_event(*c, tag->kind == Tag::kUpstream, ready[i]);  // may have closed earlier in this batch
            }
            for (Connection* c : dead_) delete c;  // only now: later events in the batch could still name them
            dead_.clear();

            if (now_ms_ - last_sweep >= 100) {
                sweep();
                last_sweep = now_ms_;
            }
            if (proxy_.stopping()) {
                if (drain_deadline < 0) {
                    drain_deadline = now_ms_ + config_.drain_ms;
                    listen(false);
                }
                if (live_.empty() || now_ms_ >= drain_deadline) {
                    while (!live_.empty()) finish(*live_.back(), CloseReason::kShutdown);
                    for (Connection* c : dead_) delete c;
                    dead_.clear();
                    write_log();
                    return;
                }
            }
            write_log();
        }
    }

private:
    void listen(bool on) {
        if (listening_ == on) return;
        listening_ = on;
        for (size_t i = 0; i < proxy_.routes_.size(); ++i) {
            int fd = proxy_.routes_[i].listen_fd;
            if (on)
                poller_.add(fd, &listener_tags_[i], kRead, /*exclusive=*/true);
            else
                poller_.remove(fd, kRead);
        }
    }

    void accept_from(Route& route) {
        // A bounded batch, so a flood of new connections cannot starve open ones.
        for (int k = 0; k < 64; ++k) {
            sockaddr_storage peer{};
            int fd = accept_client(route.listen_fd, peer);
            if (fd < 0) {
                if (errno == ECONNABORTED || errno == EINTR) continue;
                if (errno == EMFILE || errno == ENFILE) {
                    // Out of file descriptors. The listener would stay readable and spin
                    // the loop, so stop watching it until the next sweep.
                    listen(false);
                    paused_accepting_ = true;
                }
                return;  // EAGAIN: the queue is empty, or another worker took it
            }
            auto* c = new Connection(route, fd, config_.buffer_bytes, peer, now_);
            c->index = live_.size();
            live_.push_back(c);
            proxy_.totals_.accepted.fetch_add(1, std::memory_order_relaxed);
            poller_.add(fd, &c->client_tag, kRead);
            c->client_interest = kRead;  // read while connecting: a retry can resend it
            start_connect(*c);
            if (!c->closed) update(*c);
        }
    }

    // Picks a backend and starts connecting to it, or closes the client if
    // every attempt it is allowed has been used.
    void start_connect(Connection& c) {
        Balancer& balancer = *c.route->pool->balancer;
        while (c.attempts < config_.max_attempts) {
            int b = balancer.pick(c.key, now_ms_, c.tried);
            if (b == Balancer::kNone) break;  // every backend has been tried
            c.backend = c.last_backend = b;
            c.tried |= uint64_t{1} << b;
            if (++c.attempts > 1) proxy_.totals_.retries.fetch_add(1, std::memory_order_relaxed);
            int fd = connect_to(c.route->pool->backends[static_cast<size_t>(b)]);
            if (fd < 0) {  // failed at once, as with no route to the host
                attempt_failed(c, false);
                continue;
            }
            c.upstream_fd = fd;
            c.connecting = true;
            c.connect_started_ms = now_ms_;
            poller_.add(fd, &c.upstream_tag, kWrite);  // writable means the connect finished
            c.upstream_interest = kWrite;
            return;
        }
        proxy_.totals_.failed_clients.fetch_add(1, std::memory_order_relaxed);
        finish(c, c.attempts == 0  ? CloseReason::kNoBackend
                  : c.timed_out ? CloseReason::kConnectTimeout
                                : CloseReason::kConnectFailed);
    }

    // Charges a failed attempt to its backend and gives the slot back.
    void attempt_failed(Connection& c, bool timed_out) {
        Balancer& balancer = *c.route->pool->balancer;
        balancer.failed(c.backend, now_ms_);
        balancer.release(c.backend);
        c.backend = Balancer::kNone;
        c.timed_out = timed_out;
        proxy_.totals_.failed_connects.fetch_add(1, std::memory_order_relaxed);
    }

    // The current attempt failed: nothing has been sent to the backend yet, so
    // try another one with the client's bytes still buffered.
    void retry(Connection& c, bool timed_out) {
        attempt_failed(c, timed_out);
        ::close(c.upstream_fd);  // also removes it from the poller
        c.upstream_fd = -1;
        c.upstream_interest = kNone;
        c.connecting = false;
        start_connect(c);
        if (!c.closed) update(c);
    }

    void on_event(Connection& c, bool upstream_side, const Ready& r) {
        if (upstream_side && c.connecting) {
            if (!r.error && connect_result(c.upstream_fd) == 0) {
                c.connecting = false;
                c.connect_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(now_ - c.accepted_at).count();
                c.route->pool->balancer->connected(c.backend);
                flush(c, true);  // whatever the client sent while we were connecting
                if (!c.closed) update(c);
            } else {
                retry(c, false);
            }
            return;
        }
        if (r.error) {
            finish(c, upstream_side ? CloseReason::kBackendReset : CloseReason::kClientReset);
            return;
        }
        if (r.readable) pump(c, upstream_side);
        if (r.writable && !c.closed) flush(c, upstream_side);
        if (!c.closed) update(c);
    }

    // One read from one side into the buffer towards the other.
    void pump(Connection& c, bool from_upstream) {
        int fd = from_upstream ? c.upstream_fd : c.client_fd;
        Buffer& buffer = from_upstream ? c.down : c.up;
        bool& eof = from_upstream ? c.upstream_eof : c.client_eof;
        if (eof || !buffer.has_space()) return;
        ssize_t n = buffer.fill(fd);
        ++c.reads;
        if (n > 0) {
            (from_upstream ? c.bytes_down : c.bytes_up) += static_cast<uint64_t>(n);
            c.last_active_ms = now_ms_;
            // Pass it on now. The other socket is almost always writable, and
            // waiting for the poller to say so would cost a trip through epoll_wait.
            flush(c, !from_upstream);
        } else if (n == 0) {
            eof = true;
            flush(c, !from_upstream);  // may pass the EOF on
        } else if (!would_block()) {
            finish(c, from_upstream ? CloseReason::kBackendReset : CloseReason::kClientReset);
        }
    }

    // Writes what is buffered towards one side; once its source has finished
    // and everything is delivered, passes the EOF on with shutdown().
    void flush(Connection& c, bool to_upstream) {
        if (to_upstream && (c.connecting || c.upstream_fd < 0)) return;  // held until connected
        int fd = to_upstream ? c.upstream_fd : c.client_fd;
        Buffer& buffer = to_upstream ? c.up : c.down;
        if (!buffer.empty()) {
            ssize_t n = buffer.drain(fd);
            ++c.writes;
            if (n < 0 && !would_block()) {
                finish(c, to_upstream ? CloseReason::kBackendReset : CloseReason::kClientReset);
                return;
            }
            if (n > 0) c.last_active_ms = now_ms_;
        }
        bool source_done = to_upstream ? c.client_eof : c.upstream_eof;
        bool& shut = to_upstream ? c.upstream_shut : c.client_shut;
        if (source_done && buffer.empty() && !shut) {
            ::shutdown(fd, SHUT_WR);
            shut = true;
            if (c.client_shut && c.upstream_shut) finish(c, CloseReason::kDone);
        }
    }

    // Watch for exactly what each socket can do next. Reading stops while the
    // buffer it fills is full, so a slow reader slows its writer: backpressure.
    void update(Connection& c) {
        uint8_t client = kNone;
        if (!c.client_eof && c.up.has_space()) client |= kRead;
        if (!c.down.empty()) client |= kWrite;
        poller_.modify(c.client_fd, &c.client_tag, c.client_interest, client);
        c.client_interest = client;

        if (c.upstream_fd < 0) return;
        uint8_t upstream = kNone;
        if (c.connecting) {
            upstream = kWrite;
        } else {
            if (!c.upstream_eof && c.down.has_space()) upstream |= kRead;
            if (!c.up.empty()) upstream |= kWrite;
        }
        poller_.modify(c.upstream_fd, &c.upstream_tag, c.upstream_interest, upstream);
        c.upstream_interest = upstream;
    }

    void finish(Connection& c, CloseReason reason) {
        if (c.closed) return;
        c.closed = true;
        if (c.backend != Balancer::kNone) c.route->pool->balancer->release(c.backend);
        ::close(c.client_fd);
        if (c.upstream_fd >= 0) ::close(c.upstream_fd);

        Totals& totals = proxy_.totals_;
        totals.closed.fetch_add(1, std::memory_order_relaxed);
        totals.bytes_up.fetch_add(c.bytes_up, std::memory_order_relaxed);
        totals.bytes_down.fetch_add(c.bytes_down, std::memory_order_relaxed);

        if (proxy_.log_fd_ >= 0) {
            ConnectionRecord record;
            record.start_unix_ms = c.start_unix_ms;
            record.worker = id_;
            record.route = c.route->listen.text;
            record.client = c.client_text;
            if (c.last_backend != Balancer::kNone)
                record.backend = c.route->pool->backends[static_cast<size_t>(c.last_backend)].text;
            record.attempts = c.attempts;
            record.connect_us = c.connect_us;
            record.duration_us =
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - c.accepted_at).count();
            record.bytes_up = c.bytes_up;
            record.bytes_down = c.bytes_down;
            record.reads = c.reads;
            record.writes = c.writes;
            record.reason = reason;
            append_json(log_, record);
            if (log_.size() > 64 * 1024) write_log();
        }

        Connection* last = live_.back();
        live_[c.index] = last;
        last->index = c.index;
        live_.pop_back();
        dead_.push_back(&c);
    }

    // Timeouts, checked by scanning every connection ten times a second.
    // ponytail: O(connections) per sweep; a timer wheel if a worker ever holds
    // hundreds of thousands of idle connections.
    void sweep() {
        if (paused_accepting_ && !proxy_.stopping()) {
            paused_accepting_ = false;
            listen(true);
        }
        for (size_t i = 0; i < live_.size();) {
            Connection* c = live_[i];
            if (c->connecting && now_ms_ - c->connect_started_ms >= config_.connect_timeout_ms)
                retry(*c, true);
            else if (now_ms_ - c->last_active_ms >= config_.idle_timeout_ms)
                finish(*c, CloseReason::kIdleTimeout);
            if (i < live_.size() && live_[i] == c) ++i;  // finish() moved another one into slot i
        }
    }

    // Telemetry lines are batched per loop iteration: one write() for many connections.
    void write_log() {
        size_t done = 0;
        while (done < log_.size()) {
            ssize_t n = ::write(proxy_.log_fd_, log_.data() + done, log_.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;  // a broken log must not take the proxy down
            }
            done += static_cast<size_t>(n);
        }
        log_.clear();
    }

    Proxy& proxy_;
    const Config& config_;
    int id_;
    Poller poller_;
    std::vector<Tag> listener_tags_;
    std::vector<Connection*> live_;
    std::vector<Connection*> dead_;
    std::string log_;
    bool listening_ = false;
    bool paused_accepting_ = false;
    Clock::time_point now_ = Clock::now();
    int64_t now_ms_ = ms_since_epoch(now_);
};

Proxy::Proxy(Config config) : config_(std::move(config)) {
    for (const auto& p : config_.pools) {
        auto pool = std::make_unique<Pool>();
        pool->name = p.name;
        for (const auto& b : p.backends) {
            auto address = resolve(b);
            if (!address) throw std::runtime_error("cannot resolve backend " + b);
            pool->backends.push_back(*address);
        }
        pool->balancer =
            std::make_unique<Balancer>(pool->backends.size(), p.strategy, config_.health);
        pools_.push_back(std::move(pool));
    }
    for (const auto& r : config_.routes) {
        auto address = resolve(r.listen);
        if (!address) throw std::runtime_error("cannot resolve listen address " + r.listen);
        Route route;
        route.listen = *address;
        for (auto& pool : pools_)
            if (pool->name == r.pool) route.pool = pool.get();
        routes_.push_back(std::move(route));
    }
    for (auto& route : routes_) route.listen_fd = listen_on(route.listen);
    if (config_.log == "-") {
        log_fd_ = STDOUT_FILENO;
    } else if (!config_.log.empty()) {
        log_fd_ = ::open(config_.log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (log_fd_ < 0) throw std::runtime_error("cannot open log file " + config_.log);
        own_log_fd_ = true;
    }
}

Proxy::~Proxy() {
    for (auto& route : routes_)
        if (route.listen_fd >= 0) ::close(route.listen_fd);
    if (own_log_fd_) ::close(log_fd_);
}

int Proxy::workers() const {
    if (config_.workers > 0) return config_.workers;
    return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
}

uint16_t Proxy::port(size_t route) const { return local_port(routes_[route].listen_fd); }

void Proxy::run() {
    std::vector<std::thread> threads;
    for (int i = 0; i < workers(); ++i) {
        threads.emplace_back([this, i] {
            try {
                Worker(*this, i).run();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "tcp-proxy: worker %d failed: %s\n", i, e.what());
                stop();
            }
        });
    }
    for (auto& t : threads) t.join();
}

}  // namespace proxy
