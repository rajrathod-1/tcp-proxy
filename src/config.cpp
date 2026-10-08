// SPDX-License-Identifier: MIT
#include "proxy/config.hpp"

#include <charconv>
#include <stdexcept>

namespace proxy {

namespace {

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

}  // namespace

Config parse_config(std::string_view text) {
    Config config;
    int line_no = 0;
    auto fail = [&](const std::string& message) -> void {
        throw std::runtime_error("line " + std::to_string(line_no) + ": " + message);
    };

    while (!text.empty()) {
        ++line_no;
        size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        auto w = words(line);
        if (w.empty()) continue;

        auto number = [&](int min, int max) {
            if (w.size() != 2) fail("`" + std::string(w[0]) + "` takes one number");
            int value = 0;
            auto [end, err] = std::from_chars(w[1].data(), w[1].data() + w[1].size(), value);
            if (err != std::errc{} || end != w[1].data() + w[1].size() || value < min || value > max)
                fail("`" + std::string(w[0]) + "` must be a whole number from " +
                     std::to_string(min) + " to " + std::to_string(max));
            return value;
        };

        std::string_view directive = w[0];
        if (directive == "workers") {
            config.workers = number(0, 1024);
        } else if (directive == "buffer_kb") {
            config.buffer_bytes = static_cast<size_t>(number(1, 16384)) * 1024;
        } else if (directive == "connect_timeout_ms") {
            config.connect_timeout_ms = number(1, 600'000);
        } else if (directive == "idle_timeout_s") {
            config.idle_timeout_ms = number(1, 86'400) * 1000;
        } else if (directive == "drain_s") {
            config.drain_ms = number(0, 3600) * 1000;
        } else if (directive == "retries") {
            config.max_attempts = number(0, 63) + 1;
        } else if (directive == "eject_after") {
            config.health.eject_after = number(1, 1000);
        } else if (directive == "eject_for_s") {
            config.health.eject_for_ms = int64_t{number(0, 86'400)} * 1000;
        } else if (directive == "log") {
            if (w.size() != 2) fail("`log` takes a file name, or - for standard output");
            config.log = std::string(w[1]);
        } else if (directive == "pool") {
            if (w.size() < 4) fail("usage: pool <name> <strategy> <host:port>...");
            for (const auto& p : config.pools)
                if (p.name == w[1]) fail("pool `" + std::string(w[1]) + "` is defined twice");
            auto strategy = parse_strategy(w[2]);
            if (!strategy)
                fail("unknown strategy `" + std::string(w[2]) +
                     "`; use round-robin, least-connections or source-hash");
            if (w.size() - 3 > Balancer::kMaxBackends)
                fail("a pool can have at most " + std::to_string(Balancer::kMaxBackends) + " backends");
            PoolConfig pool{std::string(w[1]), *strategy, {}};
            for (size_t i = 3; i < w.size(); ++i) pool.backends.emplace_back(w[i]);
            config.pools.push_back(std::move(pool));
        } else if (directive == "listen") {
            if (w.size() != 3) fail("usage: listen <host:port> <pool>");
            config.routes.push_back({std::string(w[1]), std::string(w[2])});
        } else {
            fail("unknown directive `" + std::string(directive) + "`");
        }
    }

    line_no = 0;  // what follows is about the file as a whole
    if (config.routes.empty()) throw std::runtime_error("nothing to do: no `listen` line");
    for (const auto& route : config.routes) {
        bool found = false;
        for (const auto& pool : config.pools) found = found || pool.name == route.pool;
        if (!found)
            throw std::runtime_error("`listen " + route.listen + "` sends to pool `" + route.pool +
                                     "`, which is not defined");
    }
    return config;
}

}  // namespace proxy
