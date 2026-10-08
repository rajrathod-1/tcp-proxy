// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "proxy/config.hpp"

using proxy::parse_config;

namespace {

std::string error_of(const std::string& text) {
    try {
        parse_config(text);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return "no error";
}

}  // namespace

TEST(Config, ParsesEveryDirective) {
    auto c = parse_config(R"(
        # two routes, two pools
        workers 4
        buffer_kb 32
        connect_timeout_ms 1500
        idle_timeout_s 60
        drain_s 2
        retries 2
        eject_after 3
        eject_for_s 10
        log /tmp/proxy.log

        pool web least-connections 10.0.0.1:80 10.0.0.2:80   # trailing comment
        pool db source-hash 10.0.1.1:5432
        listen 0.0.0.0:8080 web
        listen 127.0.0.1:6432 db
    )");
    EXPECT_EQ(c.workers, 4);
    EXPECT_EQ(c.buffer_bytes, 32u * 1024);
    EXPECT_EQ(c.connect_timeout_ms, 1500);
    EXPECT_EQ(c.idle_timeout_ms, 60'000);
    EXPECT_EQ(c.drain_ms, 2000);
    EXPECT_EQ(c.max_attempts, 3);
    EXPECT_EQ(c.health.eject_after, 3);
    EXPECT_EQ(c.health.eject_for_ms, 10'000);
    EXPECT_EQ(c.log, "/tmp/proxy.log");
    ASSERT_EQ(c.pools.size(), 2u);
    EXPECT_EQ(c.pools[0].name, "web");
    EXPECT_EQ(c.pools[0].strategy, proxy::Strategy::kLeastConnections);
    EXPECT_EQ(c.pools[0].backends, (std::vector<std::string>{"10.0.0.1:80", "10.0.0.2:80"}));
    EXPECT_EQ(c.pools[1].strategy, proxy::Strategy::kSourceHash);
    ASSERT_EQ(c.routes.size(), 2u);
    EXPECT_EQ(c.routes[1].listen, "127.0.0.1:6432");
    EXPECT_EQ(c.routes[1].pool, "db");
}

TEST(Config, DefaultsAreSensible) {
    auto c = parse_config("pool p round-robin 127.0.0.1:1\nlisten :8080 p\n");
    EXPECT_EQ(c.workers, 0);
    EXPECT_EQ(c.buffer_bytes, 16u * 1024);
    EXPECT_EQ(c.max_attempts, 3);
    EXPECT_TRUE(c.log.empty());
}

TEST(Config, ErrorsNameTheLine) {
    EXPECT_EQ(error_of("workers 2\nfrobnicate 1\n"), "line 2: unknown directive `frobnicate`");
    EXPECT_EQ(error_of("pool p fastest 127.0.0.1:1\n"),
              "line 1: unknown strategy `fastest`; use round-robin, least-connections or source-hash");
    EXPECT_EQ(error_of("workers many\n"), "line 1: `workers` must be a whole number from 0 to 1024");
    EXPECT_EQ(error_of("buffer_kb 0\n"), "line 1: `buffer_kb` must be a whole number from 1 to 16384");
    EXPECT_EQ(error_of("pool p round-robin\n"), "line 1: usage: pool <name> <strategy> <host:port>...");
    EXPECT_EQ(error_of("pool p round-robin a:1\npool p round-robin b:1\n"), "line 2: pool `p` is defined twice");
    EXPECT_EQ(error_of("listen :80\n"), "line 1: usage: listen <host:port> <pool>");
}

TEST(Config, ChecksTheFileAsAWhole) {
    EXPECT_EQ(error_of("pool p round-robin 127.0.0.1:1\n"), "nothing to do: no `listen` line");
    EXPECT_EQ(error_of("listen :80 missing\n"), "`listen :80` sends to pool `missing`, which is not defined");
}

TEST(Config, TheExampleFileIsValid) {
    std::ifstream in(PROXY_SOURCE_DIR "/proxy.conf.example");
    ASSERT_TRUE(in);
    std::stringstream text;
    text << in.rdbuf();
    auto c = parse_config(text.str());
    EXPECT_EQ(c.pools.size(), 2u);
    EXPECT_EQ(c.routes.size(), 2u);
    EXPECT_EQ(c.log, "-");
}
