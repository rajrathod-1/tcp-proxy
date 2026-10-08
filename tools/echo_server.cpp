// SPDX-License-Identifier: MIT
//
// A backend for load tests. Echoes every byte back; with --delay-ms it holds
// each chunk that long first, to play a slow server; with --sink it swallows
// everything, for one-way throughput.
//
//   echo-server [--listen 127.0.0.1:9000] [--threads 2] [--delay-ms 0] [--sink]

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "proxy/net.hpp"
#include "proxy/poller.hpp"

namespace {

using Clock = std::chrono::steady_clock;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct Options {
    std::string listen = "127.0.0.1:9000";
    int threads = 2;
    int delay_ms = 0;
    bool sink = false;
};

struct Conn {
    int fd;
    uint64_t generation;
    std::vector<char> buffer = std::vector<char>(64 * 1024);
    size_t length = 0;  // bytes waiting to be echoed
    size_t offset = 0;  // of which already sent
    uint8_t interest = proxy::kRead;
};

struct Delayed {
    int64_t due_ms;
    int fd;
    uint64_t generation;
};

void serve(int listen_fd, const Options& options) {
    proxy::Poller poller;
    char listener_marker = 0;
    poller.add(listen_fd, &listener_marker, proxy::kRead, true);
    std::vector<std::unique_ptr<Conn>> by_fd;
    std::deque<Delayed> delayed;  // the delay is fixed, so arrival order is due order
    uint64_t generation = 0;
    char scratch[64 * 1024];

    auto close_conn = [&](Conn& c) {
        int fd = c.fd;
        ::close(fd);
        by_fd[static_cast<size_t>(fd)].reset();
    };
    auto set_interest = [&](Conn& c, uint8_t interest) {
        poller.modify(c.fd, reinterpret_cast<void*>(static_cast<intptr_t>(c.fd) + 1), c.interest, interest);
        c.interest = interest;
    };
    auto send_pending = [&](Conn& c) {
        long n = proxy::send_some(c.fd, c.buffer.data() + c.offset, c.length - c.offset);
        if (n < 0 && errno != EAGAIN && errno != EINTR) return close_conn(c);
        if (n > 0) c.offset += static_cast<size_t>(n);
        if (c.offset == c.length) {
            c.length = c.offset = 0;
            set_interest(c, proxy::kRead);
        } else {
            set_interest(c, proxy::kWrite);
        }
    };

    proxy::Ready ready[256];
    for (;;) {
        int timeout = 100;
        if (!delayed.empty())
            timeout = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, delayed.front().due_ms - now_ms())));
        int n = poller.wait(ready, 256, timeout);
        for (int i = 0; i < n; ++i) {
            if (ready[i].tag == &listener_marker) {
                for (;;) {
                    sockaddr_storage peer{};
                    int fd = proxy::accept_client(listen_fd, peer);
                    if (fd < 0) break;
                    if (by_fd.size() <= static_cast<size_t>(fd)) by_fd.resize(static_cast<size_t>(fd) * 2 + 16);
                    by_fd[static_cast<size_t>(fd)] = std::make_unique<Conn>(Conn{fd, ++generation});
                    poller.add(fd, reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1), proxy::kRead);
                }
                continue;
            }
            auto fd = static_cast<size_t>(reinterpret_cast<intptr_t>(ready[i].tag) - 1);
            if (fd >= by_fd.size() || !by_fd[fd]) continue;
            Conn& c = *by_fd[fd];
            if (ready[i].error) {
                close_conn(c);
                continue;
            }
            if (ready[i].writable && c.length > 0) {
                send_pending(c);
                continue;
            }
            if (!ready[i].readable || c.length > 0) continue;
            if (options.sink) {
                long got = ::read(c.fd, scratch, sizeof scratch);
                if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) close_conn(c);
                continue;
            }
            long got = ::read(c.fd, c.buffer.data(), c.buffer.size());
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                close_conn(c);
                continue;
            }
            if (got < 0) continue;
            c.length = static_cast<size_t>(got);
            c.offset = 0;
            if (options.delay_ms > 0) {
                set_interest(c, proxy::kNone);  // one chunk in flight at a time
                delayed.push_back({now_ms() + options.delay_ms, c.fd, c.generation});
            } else {
                send_pending(c);
            }
        }
        int64_t now = now_ms();
        while (!delayed.empty() && delayed.front().due_ms <= now) {
            Delayed d = delayed.front();
            delayed.pop_front();
            auto fd = static_cast<size_t>(d.fd);
            if (fd < by_fd.size() && by_fd[fd] && by_fd[fd]->generation == d.generation) send_pending(*by_fd[fd]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--listen") options.listen = value();
        else if (arg == "--threads") options.threads = std::stoi(value());
        else if (arg == "--delay-ms") options.delay_ms = std::stoi(value());
        else if (arg == "--sink") options.sink = true;
        else {
            std::fputs("usage: echo-server [--listen HOST:PORT] [--threads N] [--delay-ms D] [--sink]\n", stderr);
            return 2;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    auto address = proxy::resolve(options.listen);
    if (!address) {
        std::fprintf(stderr, "echo-server: cannot resolve %s\n", options.listen.c_str());
        return 1;
    }
    int fd = proxy::listen_on(*address);
    std::vector<std::thread> threads;
    for (int i = 0; i < options.threads; ++i) threads.emplace_back(serve, fd, std::cref(options));
    for (auto& t : threads) t.join();
}
