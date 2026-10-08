// SPDX-License-Identifier: MIT
//
// End to end: the real proxy, on real loopback sockets, in front of small
// blocking test servers that share no code with it.

#include <gtest/gtest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "proxy/net.hpp"
#include "proxy/proxy.hpp"

using namespace std::chrono_literals;

namespace {

void blocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK); }

bool send_all(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = proxy::send_some(fd, data.data() + done, data.size() - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

std::string recv_exact(int fd, size_t n) {
    std::string out(n, '\0');
    size_t done = 0;
    while (done < n) {
        ssize_t got = ::read(fd, out.data() + done, n - done);
        if (got <= 0) break;
        done += static_cast<size_t>(got);
    }
    out.resize(done);
    return out;
}

std::string recv_until_eof(int fd) {
    std::string out;
    char chunk[65536];
    for (;;) {
        ssize_t got = ::read(fd, chunk, sizeof chunk);
        if (got <= 0) return out;
        out.append(chunk, static_cast<size_t>(got));
    }
}

// A blocking server with a thread per connection, running `handler` on each.
class TestServer {
public:
    explicit TestServer(std::function<void(int)> handler) {
        fd_ = proxy::listen_on(*proxy::resolve("127.0.0.1:0"));
        port_ = proxy::local_port(fd_);
        acceptor_ = std::thread([this, handler] {
            while (!stopping_) {
                pollfd p{fd_, POLLIN, 0};
                if (poll(&p, 1, 20) <= 0) continue;
                int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) continue;
                blocking(c);  // accepted sockets inherit O_NONBLOCK on BSD
                ++accepted_;
                std::lock_guard<std::mutex> lock(mutex_);
                open_.push_back(c);
                workers_.emplace_back([c, handler] {
                    handler(c);
                    // SHUT_WR, not SHUT_RDWR: after the peer's FIN, macOS fails SHUT_RDWR
                    // with ENOTCONN and sends no FIN at all.
                    ::shutdown(c, SHUT_WR);
                });
            }
        });
    }
    ~TestServer() {
        stopping_ = true;
        acceptor_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int c : open_) ::shutdown(c, SHUT_RDWR);  // unblock handlers still reading
        }
        for (auto& t : workers_) t.join();
        for (int c : open_) ::close(c);
        ::close(fd_);
    }
    uint16_t port() const { return port_; }
    int accepted() const { return accepted_; }

private:
    int fd_;
    uint16_t port_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    std::mutex mutex_;
    std::vector<int> open_;
    std::vector<std::thread> workers_;
    std::thread acceptor_;
};

void echo(int fd) {
    char chunk[65536];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n <= 0 || !send_all(fd, std::string(chunk, static_cast<size_t>(n)))) return;
    }
}

// A port with nothing listening on it: connecting is refused.
uint16_t dead_port() {
    int fd = proxy::listen_on(*proxy::resolve("127.0.0.1:0"));
    uint16_t port = proxy::local_port(fd);
    ::close(fd);
    return port;
}

std::string local(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

proxy::Config config_for(std::vector<uint16_t> backends, proxy::Strategy strategy = proxy::Strategy::kRoundRobin) {
    proxy::Config config;
    config.workers = 2;
    config.drain_ms = 300;
    proxy::PoolConfig pool{"pool", strategy, {}};
    for (auto port : backends) pool.backends.push_back(local(port));
    config.pools.push_back(pool);
    config.routes.push_back({"127.0.0.1:0", "pool"});
    return config;
}

// The proxy running on its own thread for the length of a test.
struct Running {
    explicit Running(const proxy::Config& config)
        : proxy(std::make_unique<proxy::Proxy>(config)), thread([this] { proxy->run(); }) {}
    ~Running() {
        proxy->stop();
        thread.join();
    }
    int dial() const {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(proxy->port());
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
        return fd;
    }
    std::unique_ptr<proxy::Proxy> proxy;
    std::thread thread;
};

std::string random_bytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(rng());
    return s;
}

class ProxyTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { std::signal(SIGPIPE, SIG_IGN); }
};

}  // namespace

TEST_F(ProxyTest, RelaysEveryByteBothWays) {
    TestServer backend(echo);
    Running running(config_for({backend.port()}));
    int fd = running.dial();
    std::string payload = random_bytes(4 << 20, 1);  // more than every buffer on the way
    std::thread writer([&] {
        send_all(fd, payload);
        ::shutdown(fd, SHUT_WR);
    });
    std::string back = recv_until_eof(fd);
    writer.join();
    ::close(fd);
    EXPECT_EQ(back.size(), payload.size());
    EXPECT_TRUE(back == payload);
}

TEST_F(ProxyTest, ManyClientsAtOnce) {
    TestServer backend(echo);
    Running running(config_for({backend.port()}));
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 64; ++t) {
        clients.emplace_back([&, t] {
            int fd = running.dial();
            bool good = true;
            for (int i = 0; i < 20; ++i) {
                std::string message = random_bytes(1 + static_cast<size_t>((t * 31 + i * 7) % 3000), static_cast<unsigned>(t * 100 + i));
                good = good && send_all(fd, message) && recv_exact(fd, message.size()) == message;
            }
            ::close(fd);
            ok += good;
        });
    }
    for (auto& c : clients) c.join();
    EXPECT_EQ(ok, 64);
}

TEST_F(ProxyTest, HalfCloseReachesTheBackend) {
    // The backend answers only once the client has finished sending.
    TestServer backend([](int fd) {
        std::string all = recv_until_eof(fd);
        send_all(fd, "got " + std::to_string(all.size()));
    });
    Running running(config_for({backend.port()}));
    int fd = running.dial();
    send_all(fd, std::string(100'000, 'a'));
    ::shutdown(fd, SHUT_WR);
    EXPECT_EQ(recv_until_eof(fd), "got 100000");
    ::close(fd);
}

TEST_F(ProxyTest, BackendCloseReachesTheClient) {
    TestServer backend([](int fd) { send_all(fd, "bye"); });
    Running running(config_for({backend.port()}));
    int fd = running.dial();
    EXPECT_EQ(recv_until_eof(fd), "bye");
    ::close(fd);
}

TEST_F(ProxyTest, RetriesTheNextBackendWhenOneRefuses) {
    TestServer live(echo);
    auto config = config_for({dead_port(), live.port()});
    config.health = {1, 60'000};
    Running running(config);
    for (int i = 0; i < 10; ++i) {
        int fd = running.dial();
        send_all(fd, "ping");
        EXPECT_EQ(recv_exact(fd, 4), "ping") << "connection " << i;
        ::close(fd);
    }
    // The dead backend is tried once, ejected, and then skipped.
    EXPECT_EQ(running.proxy->totals().failed_connects, 1u);
    EXPECT_EQ(running.proxy->totals().retries, 1u);
    EXPECT_EQ(live.accepted(), 10);
}

TEST_F(ProxyTest, ClosesTheClientWhenNoBackendAnswers) {
    Running running(config_for({dead_port(), dead_port()}));
    int fd = running.dial();
    send_all(fd, "anyone?");
    EXPECT_EQ(recv_until_eof(fd), "");
    ::close(fd);
    for (int i = 0; i < 100 && running.proxy->totals().failed_clients == 0; ++i) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(running.proxy->totals().failed_clients, 1u);
    EXPECT_EQ(running.proxy->totals().failed_connects, 2u);
}

TEST_F(ProxyTest, RoundRobinSpreadsConnections) {
    TestServer a(echo), b(echo), c(echo);
    Running running(config_for({a.port(), b.port(), c.port()}));
    for (int i = 0; i < 30; ++i) {
        int fd = running.dial();
        send_all(fd, "x");
        recv_exact(fd, 1);
        ::close(fd);
    }
    EXPECT_EQ(a.accepted(), 10);
    EXPECT_EQ(b.accepted(), 10);
    EXPECT_EQ(c.accepted(), 10);
}

TEST_F(ProxyTest, LeastConnectionsBalancesOpenConnections) {
    TestServer a(echo), b(echo), c(echo);
    Running running(config_for({a.port(), b.port(), c.port()}, proxy::Strategy::kLeastConnections));
    std::vector<int> open;
    for (int i = 0; i < 9; ++i) {
        int fd = running.dial();
        send_all(fd, "x");
        recv_exact(fd, 1);  // established end to end
        open.push_back(fd);
    }
    const auto& balancer = *running.proxy->pool(0).balancer;
    for (int i = 0; i < 3; ++i) EXPECT_EQ(balancer.active(i), 3);
    for (int fd : open) ::close(fd);
    for (int k = 0; k < 200 && balancer.active(0) + balancer.active(1) + balancer.active(2) > 0; ++k)
        std::this_thread::sleep_for(5ms);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(balancer.active(i), 0);
}

TEST_F(ProxyTest, IdleConnectionsAreClosed) {
    TestServer backend(echo);
    auto config = config_for({backend.port()});
    config.idle_timeout_ms = 300;
    Running running(config);
    int fd = running.dial();
    send_all(fd, "x");
    recv_exact(fd, 1);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(recv_until_eof(fd), "");
    auto waited = std::chrono::steady_clock::now() - start;
    EXPECT_GE(waited, 250ms);
    EXPECT_LT(waited, 2s);
    ::close(fd);
}

TEST_F(ProxyTest, StopDrainsThenCloses) {
    TestServer backend(echo);
    auto running = std::make_unique<Running>(config_for({backend.port()}));
    int fd = running->dial();
    send_all(fd, "x");
    recv_exact(fd, 1);
    auto start = std::chrono::steady_clock::now();
    running.reset();  // stop(), then join: returns once the drain period is over
    auto took = std::chrono::steady_clock::now() - start;
    EXPECT_GE(took, 250ms);
    EXPECT_LT(took, 2s);
    EXPECT_EQ(recv_until_eof(fd), "");
    ::close(fd);
}

TEST_F(ProxyTest, LogsOneLinePerConnection) {
    TestServer backend(echo);
    char path[] = "/tmp/proxy-log-XXXXXX";
    int tmp = mkstemp(path);
    ::close(tmp);
    auto config = config_for({backend.port()});
    config.log = path;
    {
        Running running(config);
        int fd = running.dial();
        send_all(fd, "hello");
        recv_exact(fd, 5);
        ::shutdown(fd, SHUT_WR);
        recv_until_eof(fd);
        ::close(fd);
        for (int i = 0; i < 200 && running.proxy->totals().closed == 0; ++i) std::this_thread::sleep_for(5ms);
    }
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    std::remove(path);
    std::string line = text.str();
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
    EXPECT_NE(line.find("\"backend\":\"" + local(backend.port()) + "\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"attempts\":1"), std::string::npos) << line;
    EXPECT_NE(line.find("\"bytes_up\":5,\"bytes_down\":5"), std::string::npos) << line;
    EXPECT_NE(line.find("\"close\":\"done\""), std::string::npos) << line;
}
