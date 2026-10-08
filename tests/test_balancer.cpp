// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <thread>
#include <vector>

#include "proxy/balancer.hpp"

using proxy::Balancer;
using proxy::HealthPolicy;
using proxy::Strategy;

namespace {

std::vector<int> counts(Balancer& b, int picks, int64_t now = 0) {
    std::vector<int> n(b.size());
    for (int i = 0; i < picks; ++i) {
        int k = b.pick(static_cast<uint64_t>(i), now);
        ++n[static_cast<size_t>(k)];
        b.release(k);
    }
    return n;
}

}  // namespace

TEST(Balancer, RoundRobinCyclesInOrder) {
    Balancer b(3, Strategy::kRoundRobin);
    std::vector<int> order;
    for (int i = 0; i < 6; ++i) order.push_back(b.pick(0, 0));
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 0, 1, 2}));
}

TEST(Balancer, PickCountsActiveConnectionsUntilRelease) {
    Balancer b(2, Strategy::kRoundRobin);
    int x = b.pick(0, 0), y = b.pick(0, 0), z = b.pick(0, 0);
    EXPECT_EQ(b.active(0) + b.active(1), 3);
    b.release(x);
    b.release(y);
    b.release(z);
    EXPECT_EQ(b.active(0), 0);
    EXPECT_EQ(b.active(1), 0);
}

TEST(Balancer, LeastConnectionsPicksTheIdlest) {
    Balancer b(3, Strategy::kLeastConnections);
    for (int k = 0; k < 30; ++k) {
        int fewest = std::min({b.active(0), b.active(1), b.active(2)});
        int next = b.pick(0, 0);
        EXPECT_EQ(b.active(next) - 1, fewest);
        if (k % 4 == 3) b.release((k / 4) % 3);  // some finish, so the counts drift apart
    }
}

TEST(Balancer, LeastConnectionsSpreadsAnIdlePoolEvenly) {
    Balancer b(4, Strategy::kLeastConnections);
    auto n = counts(b, 400);  // each released at once, so every pick is a four-way tie
    for (int c : n) EXPECT_EQ(c, 100);
}

TEST(Balancer, LeastConnectionsRoutesAroundASlowBackend) {
    // Backend 2 never finishes its connections; the others finish at once.
    Balancer b(3, Strategy::kLeastConnections);
    std::vector<int> n(3);
    for (int i = 0; i < 300; ++i) {
        int k = b.pick(0, 0);
        ++n[static_cast<size_t>(k)];
        if (k != 2) b.release(k);
    }
    EXPECT_EQ(n[2], 1);  // one connection, then it is never the least loaded again
}

TEST(Balancer, SourceHashIsStickyPerClient) {
    Balancer b(5, Strategy::kSourceHash);
    for (uint64_t client = 0; client < 100; ++client) {
        int first = b.pick(client, 0);
        b.release(first);
        for (int k = 0; k < 5; ++k) {
            int again = b.pick(client, 0);
            EXPECT_EQ(again, first);
            b.release(again);
        }
    }
}

TEST(Balancer, SourceHashMovesOnlyTheClientsOfAFailedBackend) {
    Balancer b(5, Strategy::kSourceHash, HealthPolicy{1, 10'000});
    std::map<uint64_t, int> before;
    for (uint64_t client = 0; client < 2000; ++client) {
        before[client] = b.pick(client * 0x9e3779b97f4a7c15ull, 0);
        b.release(before[client]);
    }
    b.failed(3, 0);  // backend 3 is ejected
    int moved = 0;
    for (auto [client, was] : before) {
        int now = b.pick(client * 0x9e3779b97f4a7c15ull, 1);
        b.release(now);
        EXPECT_NE(now, 3);
        if (was != 3) {
            EXPECT_EQ(now, was) << "a client of a healthy backend moved";
        }
        moved += was == 3;
    }
    EXPECT_GT(moved, 300);  // about a fifth of 2000
    EXPECT_LT(moved, 500);
}

TEST(Balancer, AFailedBackendIsSkippedUntilItsEjectionEnds) {
    Balancer b(3, Strategy::kRoundRobin, HealthPolicy{1, 5000});
    b.failed(1, 1000);
    EXPECT_FALSE(b.available(1, 1000));
    auto n = counts(b, 30, 2000);
    EXPECT_EQ(n[1], 0);
    EXPECT_EQ(n[0], 15);
    n = counts(b, 30, 6000);  // 5 s later it is back
    EXPECT_EQ(n[1], 10);
}

TEST(Balancer, EjectionWaitsForTheConfiguredStreak) {
    Balancer b(2, Strategy::kRoundRobin, HealthPolicy{3, 5000});
    b.failed(0, 0);
    b.failed(0, 0);
    EXPECT_TRUE(b.available(0, 0));
    b.connected(0);  // a success resets the streak
    b.failed(0, 0);
    b.failed(0, 0);
    EXPECT_TRUE(b.available(0, 0));
    b.failed(0, 0);
    EXPECT_FALSE(b.available(0, 0));
}

TEST(Balancer, TriedBackendsAreNotPickedAgain) {
    for (auto strategy : {Strategy::kRoundRobin, Strategy::kLeastConnections, Strategy::kSourceHash}) {
        Balancer b(3, strategy);
        uint64_t tried = 0;
        std::vector<int> seen;
        for (int k = 0; k < 3; ++k) {
            int pick = b.pick(42, 0, tried);
            ASSERT_NE(pick, Balancer::kNone);
            tried |= uint64_t{1} << pick;
            seen.push_back(pick);
        }
        EXPECT_EQ(b.pick(42, 0, tried), Balancer::kNone) << proxy::name(strategy);
        std::sort(seen.begin(), seen.end());
        EXPECT_EQ(seen, (std::vector<int>{0, 1, 2})) << proxy::name(strategy);
    }
}

TEST(Balancer, WhenEveryBackendIsEjectedOneIsTriedAnyway) {
    Balancer b(2, Strategy::kRoundRobin, HealthPolicy{1, 60'000});
    b.failed(0, 0);
    b.failed(1, 0);
    EXPECT_NE(b.pick(0, 1), Balancer::kNone);
}

TEST(Balancer, ConcurrentPicksAndReleasesBalanceOut) {
    Balancer b(4, Strategy::kLeastConnections);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&b, t] {
            for (int i = 0; i < 20'000; ++i) {
                int k = b.pick(static_cast<uint64_t>(t * i), 0);
                b.release(k);
            }
        });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < 4; ++i) EXPECT_EQ(b.active(i), 0);
}

TEST(Balancer, StrategyNamesRoundTrip) {
    for (auto s : {Strategy::kRoundRobin, Strategy::kLeastConnections, Strategy::kSourceHash})
        EXPECT_EQ(proxy::parse_strategy(proxy::name(s)), s);
    EXPECT_FALSE(proxy::parse_strategy("random"));
}
