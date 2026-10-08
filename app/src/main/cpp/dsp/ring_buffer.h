// Single-producer / single-consumer byte ring buffer.
//
// The producer is the decoder thread (through JNI), the consumer is the libusb
// event thread filling isochronous packets. Positions are monotonically
// increasing 64-bit counters, so full/empty never become ambiguous.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsp {

class ByteRing {
public:
    explicit ByteRing(size_t capacity) : buf_(capacity) {}

    size_t capacity() const { return buf_.size(); }
    size_t available() const { return static_cast<size_t>(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }
    size_t space() const { return capacity() - available(); }

    // Producer side.
    size_t write(const uint8_t* src, size_t n) {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t r = r_.load(std::memory_order_acquire);
        n = std::min(n, capacity() - static_cast<size_t>(w - r));
        copyIn(w, src, n);
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer side.
    size_t read(uint8_t* dst, size_t n) {
        if (flush_.exchange(false, std::memory_order_acq_rel)) r_.store(w_.load(std::memory_order_acquire), std::memory_order_release);
        const uint64_t r = r_.load(std::memory_order_relaxed);
        const uint64_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(w - r));
        copyOut(r, dst, n);
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    // Asks the consumer to drop everything queued so far (used on seek/stop).
    void requestFlush() { flush_.store(true, std::memory_order_release); }

    // Only safe while the consumer is not running.
    void clear() {
        r_.store(w_.load());
        flush_.store(false);
    }

private:
    void copyIn(uint64_t pos, const uint8_t* src, size_t n) {
        const size_t cap = capacity();
        size_t off = static_cast<size_t>(pos % cap);
        size_t first = std::min(n, cap - off);
        std::memcpy(&buf_[off], src, first);
        if (n > first) std::memcpy(&buf_[0], src + first, n - first);
    }
    void copyOut(uint64_t pos, uint8_t* dst, size_t n) const {
        const size_t cap = capacity();
        size_t off = static_cast<size_t>(pos % cap);
        size_t first = std::min(n, cap - off);
        std::memcpy(dst, &buf_[off], first);
        if (n > first) std::memcpy(dst + first, &buf_[0], n - first);
    }

    std::vector<uint8_t> buf_;
    std::atomic<uint64_t> w_{0};
    std::atomic<uint64_t> r_{0};
    std::atomic<bool> flush_{false};
};

}  // namespace dsp
