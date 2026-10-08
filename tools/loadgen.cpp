// SPDX-License-Identifier: MIT
//
// A closed-loop load generator for echo backends, direct or through the proxy.
//
//   loadgen --target HOST:PORT [--mode rr|connect|stream] [--connections 64]
//           [--size 64] [--seconds 10] [--warmup 1] [--threads 4] [--json]
//
//   rr       each connection sends `size` bytes, waits for all of them to come
//            back, and repeats: requests per second and round-trip latency
//   connect  the same, but every request opens a new connection and closes it:
//            connection churn, with the connect inside the latency
//   stream   each connection writes `size`-byte chunks as fast as it can while
//            reading the echo: throughput
//
// Nothing is recorded during the warmup. Latencies are kept individually and
// the percentiles are exact.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "proxy/net.hpp"
#include "proxy/poller.hpp"

namespace {

using Clock = std::chrono::steady_clock;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

enum class Mode { kRequestResponse, kConnect, kStream };

struct Options {
    proxy::Address target;
    Mode mode = Mode::kRequestResponse;
    int connections = 64;
    size_t size = 64;
    double seconds = 10;
    double warmup = 1;
    int threads = 4;
    bool json = false;
};

struct Client {
    int fd = -1;
    bool connecting = false;
    uint8_t interest = proxy::kNone;
    size_t sent = 0;
    size_t received = 0;
    int64_t started_ns = 0;
};

struct Tally {
    uint64_t requests = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    std::vector<int64_t> latencies_ns;
};

void drive(const Options& o, int count, int64_t begin_ns, int64_t end_ns, Tally& tally) {
    proxy::Poller poller;
    std::vector<Client> clients(static_cast<size_t>(count));
    std::vector<char> payload(std::max<size_t>(o.size, 1), 'x');
    std::vector<char> scratch(256 * 1024);
    auto counting = [&](int64_t t) { return t >= begin_ns && t < end_ns; };

    auto set = [&](Client& c, uint8_t interest) {
        poller.modify(c.fd, &c, c.interest, interest);
        c.interest = interest;
    };
    auto open = [&](Client& c) {
        c = Client{};
        c.started_ns = now_ns();
        c.fd = proxy::connect_to(o.target);
        if (c.fd < 0) {
            if (counting(c.started_ns)) ++tally.errors;
            return;  // retried at the next pass
        }
        c.connecting = true;
        c.interest = proxy::kWrite;
        poller.add(c.fd, &c, proxy::kWrite);
    };
    auto drop = [&](Client& c, bool error) {
        if (error && counting(now_ns())) ++tally.errors;
        ::close(c.fd);
        c.fd = -1;
    };
    auto send_request = [&](Client& c) {
        while (c.sent < o.size) {
            long n = proxy::send_some(c.fd, payload.data() + c.sent, o.size - c.sent);
            if (n <= 0) break;
            c.sent += static_cast<size_t>(n);
        }
        set(c, c.sent < o.size ? proxy::kWrite : proxy::kRead);
    };

    for (auto& c : clients) open(c);
    proxy::Ready ready[256];
    while (now_ns() < end_ns) {
        for (auto& c : clients)
            if (c.fd < 0) open(c);
        int n = poller.wait(ready, 256, 50);
        for (int i = 0; i < n; ++i) {
            Client& c = *static_cast<Client*>(ready[i].tag);
            if (c.fd < 0) continue;
            if (c.connecting) {
                if (ready[i].error || proxy::connect_result(c.fd) != 0) {
                    drop(c, true);
                    continue;
                }
                c.connecting = false;
                if (o.mode == Mode::kStream) {
                    set(c, proxy::kRead | proxy::kWrite);
                } else {
                    if (o.mode == Mode::kRequestResponse) c.started_ns = now_ns();
                    send_request(c);
                }
                continue;
            }
            if (ready[i].writable) {
                if (o.mode == Mode::kStream)
                    proxy::send_some(c.fd, payload.data(), payload.size());
                else
                    send_request(c);
            }
            if (!ready[i].readable && !ready[i].error) continue;
            long got = ::read(c.fd, scratch.data(), scratch.size());
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (got <= 0) {  // the server closed or reset mid-request
                drop(c, true);
                continue;
            }
            int64_t t = now_ns();
            if (o.mode == Mode::kStream) {
                if (counting(t)) tally.bytes += static_cast<uint64_t>(got);
                continue;
            }
            c.received += static_cast<size_t>(got);
            if (c.received < o.size) continue;
            if (counting(t)) {
                ++tally.requests;
                tally.bytes += o.size;
                tally.latencies_ns.push_back(t - c.started_ns);
            }
            if (o.mode == Mode::kConnect) {
                drop(c, false);
                open(c);
            } else {
                c.sent = c.received = 0;
                c.started_ns = t;
                send_request(c);
            }
        }
    }
    for (auto& c : clients)
        if (c.fd >= 0) ::close(c.fd);
}

int usage() {
    std::fputs(
        "usage: loadgen --target HOST:PORT [--mode rr|connect|stream] [--connections N]\n"
        "               [--size BYTES] [--seconds S] [--warmup S] [--threads N] [--json]\n",
        stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string target;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--target") target = value();
        else if (arg == "--mode") {
            std::string m = value();
            if (m == "rr") o.mode = Mode::kRequestResponse;
            else if (m == "connect") o.mode = Mode::kConnect;
            else if (m == "stream") o.mode = Mode::kStream;
            else return usage();
        } else if (arg == "--connections") o.connections = std::stoi(value());
        else if (arg == "--size") o.size = std::stoul(value());
        else if (arg == "--seconds") o.seconds = std::stod(value());
        else if (arg == "--warmup") o.warmup = std::stod(value());
        else if (arg == "--threads") o.threads = std::stoi(value());
        else if (arg == "--json") o.json = true;
        else return usage();
    }
    auto address = proxy::resolve(target);
    if (!address) return usage();
    o.target = *address;
    o.threads = std::max(1, std::min(o.threads, o.connections));
    std::signal(SIGPIPE, SIG_IGN);

    int64_t begin = now_ns() + static_cast<int64_t>(o.warmup * 1e9);
    int64_t end = begin + static_cast<int64_t>(o.seconds * 1e9);
    std::vector<Tally> tallies(static_cast<size_t>(o.threads));
    std::vector<std::thread> threads;
    for (int t = 0; t < o.threads; ++t) {
        int share = o.connections / o.threads + (t < o.connections % o.threads ? 1 : 0);
        threads.emplace_back(drive, std::cref(o), share, begin, end, std::ref(tallies[static_cast<size_t>(t)]));
    }
    for (auto& t : threads) t.join();

    Tally all;
    for (auto& t : tallies) {
        all.requests += t.requests;
        all.bytes += t.bytes;
        all.errors += t.errors;
        all.latencies_ns.insert(all.latencies_ns.end(), t.latencies_ns.begin(), t.latencies_ns.end());
    }
    std::sort(all.latencies_ns.begin(), all.latencies_ns.end());
    auto pct = [&](double q) -> double {
        if (all.latencies_ns.empty()) return 0;
        size_t i = std::min(all.latencies_ns.size() - 1, static_cast<size_t>(q * static_cast<double>(all.latencies_ns.size())));
        return static_cast<double>(all.latencies_ns[i]) / 1000.0;
    };
    double rps = static_cast<double>(all.requests) / o.seconds;
    double mbps = static_cast<double>(all.bytes) / o.seconds / 1e6;
    const char* mode = o.mode == Mode::kStream ? "stream" : o.mode == Mode::kConnect ? "connect" : "rr";

    if (o.json) {
        std::printf(
            "{\"mode\":\"%s\",\"connections\":%d,\"size\":%zu,\"seconds\":%.1f,\"requests\":%llu,"
            "\"rps\":%.0f,\"mb_per_s\":%.2f,\"gbit_per_s\":%.3f,\"p50_us\":%.1f,\"p90_us\":%.1f,"
            "\"p99_us\":%.1f,\"p999_us\":%.1f,\"max_us\":%.1f,\"errors\":%llu}\n",
            mode, o.connections, o.size, o.seconds, static_cast<unsigned long long>(all.requests), rps,
            mbps, mbps * 8 / 1000, pct(0.5), pct(0.9), pct(0.99), pct(0.999), pct(1.0),
            static_cast<unsigned long long>(all.errors));
        return 0;
    }
    std::printf("%s  mode %s  %d connections  %zu-byte %s  %.1f s\n", target.c_str(), mode,
                o.connections, o.size, o.mode == Mode::kStream ? "chunks" : "requests", o.seconds);
    if (o.mode == Mode::kStream) {
        std::printf("throughput  %.2f Gbit/s echoed back\n", mbps * 8 / 1000);
    } else {
        std::printf("requests    %llu (%.0f/s)\n", static_cast<unsigned long long>(all.requests), rps);
        std::printf("latency us  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", pct(0.5),
                    pct(0.9), pct(0.99), pct(0.999), pct(1.0));
    }
    std::printf("errors      %llu\n", static_cast<unsigned long long>(all.errors));
}
