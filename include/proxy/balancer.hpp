// SPDX-License-Identifier: MIT
#pragma once

// Chooses a backend for each new connection, and takes a backend out of
// rotation when connecting to it fails (passive health checking).
//
// There are no sockets here and time is passed in, so the same code is unit
// tested, shared by the proxy's worker threads, and compiled to WebAssembly
// for the browser demo. All state is atomic: workers pick concurrently.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace proxy {

enum class Strategy : uint8_t { kRoundRobin, kLeastConnections, kSourceHash };

inline std::optional<Strategy> parse_strategy(std::string_view s) {
    if (s == "round-robin") return Strategy::kRoundRobin;
    if (s == "least-connections") return Strategy::kLeastConnections;
    if (s == "source-hash") return Strategy::kSourceHash;
    return std::nullopt;
}

inline const char* name(Strategy s) {
    switch (s) {
        case Strategy::kRoundRobin: return "round-robin";
        case Strategy::kLeastConnections: return "least-connections";
        case Strategy::kSourceHash: return "source-hash";
    }
    return "?";
}

struct HealthPolicy {
    int eject_after = 1;          // consecutive failed connects that take a backend out
    int64_t eject_for_ms = 5000;  // how long it stays out before it is tried again
};

class Balancer {
public:
    static constexpr int kNone = -1;
    static constexpr size_t kMaxBackends = 64;  // `tried` is a 64-bit mask

    Balancer(size_t backends, Strategy strategy, HealthPolicy policy = {})
        : slots_(new Slot[backends]), n_(backends), strategy_(strategy), policy_(policy) {}

    // Picks a backend that is not in `tried` (bit i set: backend i already
    // failed for this connection) and counts the connection against it, until
    // release(). Ejected backends are skipped while any other is available;
    // when every candidate is ejected, one is tried anyway, since refusing the
    // client is certain to fail and a retry might not.
    int pick(uint64_t key, int64_t now_ms, uint64_t tried = 0) {
        int b = choose(key, now_ms, tried, true);
        if (b == kNone) b = choose(key, now_ms, tried, false);
        if (b != kNone) slot(b).active.fetch_add(1, std::memory_order_relaxed);
        return b;
    }

    // The connection to `b` ended, or never connected.
    void release(int b) { slot(b).active.fetch_sub(1, std::memory_order_relaxed); }

    // A connect to `b` succeeded: its failure streak is over.
    void connected(int b) { slot(b).failures.store(0, std::memory_order_relaxed); }

    // A connect to `b` failed: refused, unreachable or timed out.
    void failed(int b, int64_t now_ms) {
        int streak = slot(b).failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak >= policy_.eject_after)
            slot(b).ejected_until.store(now_ms + policy_.eject_for_ms, std::memory_order_relaxed);
    }

    size_t size() const { return n_; }
    Strategy strategy() const { return strategy_; }
    int active(int b) const { return slot(b).active.load(std::memory_order_relaxed); }
    bool available(int b, int64_t now_ms) const {
        return slot(b).ejected_until.load(std::memory_order_relaxed) <= now_ms;
    }

private:
    int choose(uint64_t key, int64_t now_ms, uint64_t tried, bool available_only) {
        // The candidates, in index order. Rotating over these (not over every
        // backend, skipping the ineligible) keeps the share of an ejected
        // backend from all landing on the one after it.
        int candidates[kMaxBackends];
        size_t m = 0;
        for (size_t i = 0; i < n_; ++i) {
            int b = static_cast<int>(i);
            if (!((tried >> i) & 1u) && (!available_only || available(b, now_ms))) candidates[m++] = b;
        }
        if (m == 0) return kNone;
        switch (strategy_) {
            case Strategy::kRoundRobin:
                return candidates[next_.fetch_add(1, std::memory_order_relaxed) % m];
            case Strategy::kLeastConnections: {
                // Ties are broken from a rotating start, so an idle pool still spreads evenly.
                size_t start = next_.fetch_add(1, std::memory_order_relaxed) % m;
                int best = candidates[start];
                for (size_t k = 1; k < m; ++k) {
                    int b = candidates[(start + k) % m];
                    if (active(b) < active(best)) best = b;
                }
                return best;
            }
            case Strategy::kSourceHash: {
                // Rendezvous hashing: a client goes to the backend with the highest
                // score(client, backend). When a backend leaves, only the clients that
                // were on it move; everyone else keeps their backend.
                int best = candidates[0];
                uint64_t top = score(key, best);
                for (size_t k = 1; k < m; ++k) {
                    uint64_t s = score(key, candidates[k]);
                    if (s > top) {
                        best = candidates[k];
                        top = s;
                    }
                }
                return best;
            }
        }
        return kNone;
    }

    static uint64_t score(uint64_t key, int b) {
        return mix(key ^ ((static_cast<uint64_t>(b) + 1) * 0x9e3779b97f4a7c15ull));
    }

    static uint64_t mix(uint64_t x) {  // splitmix64's finaliser
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    // One cache line per backend, so workers updating different backends never share a line.
    struct alignas(64) Slot {
        std::atomic<int> active{0};
        std::atomic<int> failures{0};
        std::atomic<int64_t> ejected_until{0};
    };

    Slot& slot(int b) const { return slots_[static_cast<size_t>(b)]; }

    std::unique_ptr<Slot[]> slots_;
    size_t n_;
    Strategy strategy_;
    HealthPolicy policy_;
    std::atomic<uint64_t> next_{0};
};

}  // namespace proxy
