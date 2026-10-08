// SPDX-License-Identifier: MIT
//
// The proxy's balancer, unchanged, behind a C interface for the browser demo.
// Browsers have no raw sockets, so the page simulates backends and asks this
// code, exactly as a worker thread does, where each new connection should go
// and what to make of a failed connect.

#include <emscripten/emscripten.h>

#include <memory>
#include <vector>

#include "proxy/balancer.hpp"

namespace {
std::vector<std::unique_ptr<proxy::Balancer>> balancers;
proxy::Balancer& at(int h) { return *balancers[static_cast<size_t>(h)]; }
}  // namespace

extern "C" {

// strategy: 0 round robin, 1 least connections, 2 source hash. Returns a handle.
EMSCRIPTEN_KEEPALIVE int create(int backends, int strategy, int eject_after, double eject_for_ms) {
    balancers.push_back(std::make_unique<proxy::Balancer>(
        static_cast<size_t>(backends), static_cast<proxy::Strategy>(strategy),
        proxy::HealthPolicy{eject_after, static_cast<int64_t>(eject_for_ms)}));
    return static_cast<int>(balancers.size()) - 1;
}

EMSCRIPTEN_KEEPALIVE int pick(int h, double key, double now_ms, double tried) {
    return at(h).pick(static_cast<uint64_t>(key), static_cast<int64_t>(now_ms), static_cast<uint64_t>(tried));
}

EMSCRIPTEN_KEEPALIVE void release(int h, int b) { at(h).release(b); }
EMSCRIPTEN_KEEPALIVE void connected(int h, int b) { at(h).connected(b); }
EMSCRIPTEN_KEEPALIVE void failed(int h, int b, double now_ms) { at(h).failed(b, static_cast<int64_t>(now_ms)); }
EMSCRIPTEN_KEEPALIVE int active(int h, int b) { return at(h).active(b); }
EMSCRIPTEN_KEEPALIVE int available(int h, int b, double now_ms) {
    return at(h).available(b, static_cast<int64_t>(now_ms));
}
}
