// SPDX-License-Identifier: MIT
#pragma once

// Level-triggered readiness over epoll on Linux and kqueue on macOS, so the
// proxy is developed and tested on both. Linux is the production target.

#include <cstdint>

namespace proxy {

enum Interest : uint8_t { kNone = 0, kRead = 1, kWrite = 2 };

struct Ready {
    void* tag;
    bool readable;
    bool writable;
    bool error;  // the socket has a pending error; the owner should close it
};

class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // `exclusive` is for a listening socket shared by several pollers: on Linux
    // a new connection wakes one of them rather than all (EPOLLEXCLUSIVE).
    void add(int fd, void* tag, uint8_t interest, bool exclusive = false);
    void modify(int fd, void* tag, uint8_t from, uint8_t to);
    void remove(int fd, uint8_t interest);

    // Fills up to `max` entries; returns how many.
    int wait(Ready* out, int max, int timeout_ms);

private:
    int fd_;
};

}  // namespace proxy
