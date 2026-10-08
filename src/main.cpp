// SPDX-License-Identifier: MIT
//
//   tcp-proxy -c proxy.conf
//   tcp-proxy --listen 0.0.0.0:8080 --backend 10.0.0.1:80 --backend 10.0.0.2:80
//             [--strategy least-connections] [--workers 4] [--log -]

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "proxy/config.hpp"
#include "proxy/proxy.hpp"

namespace {

proxy::Proxy* g_proxy = nullptr;

void on_signal(int) {
    if (g_proxy) g_proxy->stop();  // an atomic store: safe in a handler
}

int usage() {
    std::fputs(
        "usage: tcp-proxy -c FILE\n"
        "       tcp-proxy --listen HOST:PORT --backend HOST:PORT [--backend HOST:PORT ...]\n"
        "                 [--strategy round-robin|least-connections|source-hash]\n"
        "                 [--workers N] [--log FILE|-]\n",
        stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    proxy::Config config;
    std::string file, listen, strategy = "round-robin";
    proxy::PoolConfig pool{"default", proxy::Strategy::kRoundRobin, {}};
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "-c" || arg == "--config") file = value();
            else if (arg == "--listen") listen = value();
            else if (arg == "--backend") pool.backends.push_back(value());
            else if (arg == "--strategy") strategy = value();
            else if (arg == "--workers") config.workers = std::stoi(value());
            else if (arg == "--log") config.log = value();
            else return usage();
        }
        if (!file.empty()) {
            std::ifstream in(file);
            if (!in) throw std::runtime_error("cannot read " + file);
            std::stringstream text;
            text << in.rdbuf();
            config = proxy::parse_config(text.str());
        } else {
            if (listen.empty() || pool.backends.empty()) return usage();
            auto parsed = proxy::parse_strategy(strategy);
            if (!parsed) throw std::runtime_error("unknown strategy " + strategy);
            pool.strategy = *parsed;
            config.pools.push_back(pool);
            config.routes.push_back({listen, "default"});
        }

        std::signal(SIGPIPE, SIG_IGN);
        proxy::Proxy proxy(config);
        g_proxy = &proxy;
        struct sigaction action {};
        action.sa_handler = on_signal;
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        for (size_t r = 0; r < config.routes.size(); ++r) {
            const auto& route = config.routes[r];
            for (const auto& p : config.pools) {
                if (p.name != route.pool) continue;
                std::fprintf(stderr, "tcp-proxy: %s -> %s (%s, %zu backends)\n", route.listen.c_str(),
                             p.name.c_str(), proxy::name(p.strategy), p.backends.size());
            }
        }
        std::fprintf(stderr, "tcp-proxy: %d workers\n", proxy.workers());
        proxy.run();

        const auto& t = proxy.totals();
        std::fprintf(stderr,
                     "tcp-proxy: stopped. %llu connections, %llu failed connects (%llu retried), "
                     "%llu clients never reached a backend, %llu bytes up, %llu bytes down\n",
                     static_cast<unsigned long long>(t.accepted.load()),
                     static_cast<unsigned long long>(t.failed_connects.load()),
                     static_cast<unsigned long long>(t.retries.load()),
                     static_cast<unsigned long long>(t.failed_clients.load()),
                     static_cast<unsigned long long>(t.bytes_up.load()),
                     static_cast<unsigned long long>(t.bytes_down.load()));
        g_proxy = nullptr;
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tcp-proxy: %s\n", e.what());
        return 1;
    }
}
