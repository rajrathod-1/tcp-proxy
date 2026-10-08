// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

#include "proxy/telemetry.hpp"

TEST(Telemetry, OneJsonLinePerConnection) {
    proxy::ConnectionRecord r;
    r.start_unix_ms = 1'791'400'000'123;  // 2026-10-07T19:06:40.123Z
    r.worker = 2;
    r.route = "0.0.0.0:8080";
    r.client = "10.1.2.3:51234";
    r.backend = "10.0.0.2:80";
    r.attempts = 2;
    r.connect_us = 1450;
    r.duration_us = 250'000;
    r.bytes_up = 512;
    r.bytes_down = 40'960;
    r.reads = 12;
    r.writes = 11;
    r.reason = proxy::CloseReason::kDone;
    std::string line;
    proxy::append_json(line, r);
    EXPECT_EQ(line,
              "{\"start\":\"2026-10-07T19:06:40.123Z\",\"worker\":2,\"route\":\"0.0.0.0:8080\","
              "\"client\":\"10.1.2.3:51234\",\"backend\":\"10.0.0.2:80\",\"attempts\":2,"
              "\"connect_us\":1450,\"duration_us\":250000,\"bytes_up\":512,\"bytes_down\":40960,"
              "\"reads\":12,\"writes\":11,\"close\":\"done\"}\n");
}

TEST(Telemetry, EscapesStrings) {
    proxy::ConnectionRecord r;
    r.backend = "we\"ird\\host:1";
    std::string line;
    proxy::append_json(line, r);
    EXPECT_NE(line.find("\"backend\":\"we\\\"ird\\\\host:1\""), std::string::npos);
}

TEST(Telemetry, EveryReasonHasAName) {
    using proxy::CloseReason;
    for (auto r : {CloseReason::kDone, CloseReason::kClientReset, CloseReason::kBackendReset,
                   CloseReason::kConnectFailed, CloseReason::kConnectTimeout, CloseReason::kNoBackend,
                   CloseReason::kIdleTimeout, CloseReason::kShutdown})
        EXPECT_STRNE(proxy::name(r), "?");
}
