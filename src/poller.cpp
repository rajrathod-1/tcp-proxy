// SPDX-License-Identifier: MIT
#include "proxy/poller.hpp"

#include <cerrno>
#include <system_error>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

namespace proxy {

namespace {
[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
constexpr int kBatch = 256;
}  // namespace

#if defined(__linux__)

namespace {
uint32_t events_for(uint8_t interest) {
    return ((interest & kRead) ? EPOLLIN : 0u) | ((interest & kWrite) ? EPOLLOUT : 0u);
}
}  // namespace

Poller::Poller() : fd_(epoll_create1(EPOLL_CLOEXEC)) {
    if (fd_ < 0) fail("epoll_create1");
}

Poller::~Poller() { ::close(fd_); }

void Poller::add(int fd, void* tag, uint8_t interest, bool exclusive) {
    epoll_event ev{};
    ev.events = events_for(interest) | (exclusive ? EPOLLEXCLUSIVE : 0u);
    ev.data.ptr = tag;
    if (epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) < 0) fail("epoll_ctl add");
}

void Poller::modify(int fd, void* tag, uint8_t from, uint8_t to) {
    if (from == to) return;
    epoll_event ev{};
    ev.events = events_for(to);
    ev.data.ptr = tag;
    if (epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev) < 0) fail("epoll_ctl mod");
}

void Poller::remove(int fd, uint8_t) { epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr); }

int Poller::wait(Ready* out, int max, int timeout_ms) {
    epoll_event events[kBatch];
    int n = epoll_wait(fd_, events, max < kBatch ? max : kBatch, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        fail("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        uint32_t e = events[i].events;
        // A hangup also reads as readable, so the owner's read() sees the EOF.
        out[i] = {events[i].data.ptr, (e & (EPOLLIN | EPOLLHUP)) != 0, (e & EPOLLOUT) != 0,
                  (e & EPOLLERR) != 0};
    }
    return n;
}

#else  // kqueue

Poller::Poller() : fd_(kqueue()) {
    if (fd_ < 0) fail("kqueue");
}

Poller::~Poller() { ::close(fd_); }

void Poller::add(int fd, void* tag, uint8_t interest, bool) { modify(fd, tag, kNone, interest); }

void Poller::modify(int fd, void* tag, uint8_t from, uint8_t to) {
    struct kevent changes[2];
    int n = 0;
    auto filter = [&](uint8_t bit, int16_t which) {
        if ((from & bit) == (to & bit)) return;
        EV_SET(&changes[n++], static_cast<uintptr_t>(fd), which,
               static_cast<uint16_t>((to & bit) ? EV_ADD : EV_DELETE), 0, 0, tag);
    };
    filter(kRead, EVFILT_READ);
    filter(kWrite, EVFILT_WRITE);
    if (n > 0 && kevent(fd_, changes, n, nullptr, 0, nullptr) < 0) fail("kevent");
}

void Poller::remove(int fd, uint8_t interest) { modify(fd, nullptr, interest, kNone); }

int Poller::wait(Ready* out, int max, int timeout_ms) {
    struct kevent events[kBatch];
    timespec timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int n = kevent(fd_, nullptr, 0, events, max < kBatch ? max : kBatch,
                   timeout_ms < 0 ? nullptr : &timeout);
    if (n < 0) {
        if (errno == EINTR) return 0;
        fail("kevent wait");
    }
    for (int i = 0; i < n; ++i) {
        out[i] = {events[i].udata, events[i].filter == EVFILT_READ,
                  events[i].filter == EVFILT_WRITE, (events[i].flags & EV_ERROR) != 0};
    }
    return n;
}

#endif

}  // namespace proxy
