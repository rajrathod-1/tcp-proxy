// SPDX-License-Identifier: MIT
#pragma once

// One direction of a connection: bytes read from one socket, not yet written
// to the other. A flat array with read and write offsets: when the writer
// catches up, both reset to zero, so a steady stream never has to move data.

#include <cstddef>
#include <cstring>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

#include "proxy/net.hpp"

namespace proxy {

class Buffer {
public:
    explicit Buffer(size_t capacity) : data_(new char[capacity]), capacity_(capacity) {}

    size_t size() const { return end_ - start_; }
    bool empty() const { return start_ == end_; }
    bool has_space() const { return end_ < capacity_ || start_ > 0; }

    // One read() into the free space. Returns its result.
    ssize_t fill(int fd) {
        if (end_ == capacity_) compact();
        ssize_t n = ::read(fd, data_.get() + end_, capacity_ - end_);
        if (n > 0) end_ += static_cast<size_t>(n);
        return n;
    }

    // One send() of everything buffered. Returns its result.
    ssize_t drain(int fd) {
        ssize_t n = send_some(fd, data_.get() + start_, size());
        if (n > 0) {
            start_ += static_cast<size_t>(n);
            if (start_ == end_) start_ = end_ = 0;
        }
        return n;
    }

private:
    void compact() {
        std::memmove(data_.get(), data_.get() + start_, size());
        end_ -= start_;
        start_ = 0;
    }

    std::unique_ptr<char[]> data_;
    size_t capacity_;
    size_t start_ = 0;
    size_t end_ = 0;
};

}  // namespace proxy
