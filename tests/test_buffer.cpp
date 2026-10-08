// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "proxy/buffer.hpp"

namespace {

struct Pair {
    int a, b;
    Pair() {
        int fds[2];
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        a = fds[0];
        b = fds[1];
        fcntl(a, F_SETFL, O_NONBLOCK);
        fcntl(b, F_SETFL, O_NONBLOCK);
    }
    ~Pair() {
        ::close(a);
        ::close(b);
    }
};

std::string read_all(int fd) {
    std::string out;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n <= 0) return out;
        out.append(chunk, static_cast<size_t>(n));
    }
}

}  // namespace

TEST(Buffer, MovesBytesFromOneSocketToAnother) {
    Pair in, out;
    proxy::Buffer buffer(16);
    ASSERT_EQ(::write(in.b, "hello", 5), 5);
    EXPECT_EQ(buffer.fill(in.a), 5);
    EXPECT_EQ(buffer.size(), 5u);
    EXPECT_EQ(buffer.drain(out.a), 5);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(read_all(out.b), "hello");
}

TEST(Buffer, StopsAtCapacity) {
    Pair in;
    proxy::Buffer buffer(8);
    ASSERT_EQ(::write(in.b, "0123456789abcdef", 16), 16);
    EXPECT_EQ(buffer.fill(in.a), 8);
    EXPECT_FALSE(buffer.has_space());
}

TEST(Buffer, ReusesSpaceAfterAPartialDrain) {
    Pair in, out;
    proxy::Buffer buffer(8);
    ASSERT_EQ(::write(in.b, "abcdefgh", 8), 8);
    ASSERT_EQ(buffer.fill(in.a), 8);

    // A peer that accepts only part of it: fill its socket buffer first.
    int small = 1;
    setsockopt(out.a, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    std::string junk(1 << 20, 'j');
    while (::write(out.a, junk.data(), junk.size()) > 0) {
    }
    EXPECT_LT(buffer.drain(out.a), 0);  // would block: nothing lost
    EXPECT_EQ(buffer.size(), 8u);
    read_all(out.b);

    // Send three bytes, then make room for three more by compaction.
    Pair out2;
    proxy::Buffer partial(8);
    ASSERT_EQ(::write(in.b, "12345678", 8), 8);
    ASSERT_EQ(partial.fill(in.a), 8);
    ASSERT_EQ(::write(in.b, "xyz", 3), 3);
    EXPECT_FALSE(partial.has_space());
    EXPECT_EQ(partial.drain(out2.a), 8);
    EXPECT_EQ(partial.fill(in.a), 3);
    EXPECT_EQ(partial.drain(out2.a), 3);
    EXPECT_EQ(read_all(out2.b), "12345678xyz");
}

TEST(Buffer, ReportsEndOfFile) {
    Pair in;
    proxy::Buffer buffer(8);
    ::shutdown(in.b, SHUT_WR);
    EXPECT_EQ(buffer.fill(in.a), 0);
}
