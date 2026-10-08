// SPDX-License-Identifier: MIT
#include "proxy/telemetry.hpp"

#include <cstdio>
#include <ctime>

namespace proxy {

const char* name(CloseReason reason) {
    switch (reason) {
        case CloseReason::kDone: return "done";
        case CloseReason::kClientReset: return "client_reset";
        case CloseReason::kBackendReset: return "backend_reset";
        case CloseReason::kConnectFailed: return "connect_failed";
        case CloseReason::kConnectTimeout: return "connect_timeout";
        case CloseReason::kNoBackend: return "no_backend";
        case CloseReason::kIdleTimeout: return "idle_timeout";
        case CloseReason::kShutdown: return "shutdown";
    }
    return "?";
}

namespace {

// Addresses and names in this file never contain quotes or backslashes, but a
// hostname in the config is user input, so escape anyway.
void append_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    out += '"';
}

}  // namespace

void append_json(std::string& out, const ConnectionRecord& r) {
    time_t seconds = static_cast<time_t>(r.start_unix_ms / 1000);
    tm utc{};
    gmtime_r(&seconds, &utc);
    char when[64];
    std::snprintf(when, sizeof when, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(r.start_unix_ms % 1000));

    out += "{\"start\":\"";
    out += when;
    out += "\",\"worker\":" + std::to_string(r.worker);
    out += ",\"route\":";
    append_string(out, r.route);
    out += ",\"client\":";
    append_string(out, r.client);
    out += ",\"backend\":";
    append_string(out, r.backend);
    out += ",\"attempts\":" + std::to_string(r.attempts);
    out += ",\"connect_us\":" + std::to_string(r.connect_us);
    out += ",\"duration_us\":" + std::to_string(r.duration_us);
    out += ",\"bytes_up\":" + std::to_string(r.bytes_up);
    out += ",\"bytes_down\":" + std::to_string(r.bytes_down);
    out += ",\"reads\":" + std::to_string(r.reads);
    out += ",\"writes\":" + std::to_string(r.writes);
    out += ",\"close\":\"";
    out += name(r.reason);
    out += "\"}\n";
}

}  // namespace proxy
