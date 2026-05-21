// pulse/buffer.hpp — Append-and-consume byte buffer for nonblocking I/O.
//
// Design rationale:
//   Naive approach: std::string with erase(0, n) after each partial read.
//   Problem: erase from the front is O(n) memmove. At 100k req/s with even
//   modest buffer sizes, this dominates CPU.
//
//   Better: maintain read/write cursors into a contiguous byte vector and
//   periodically compact when the read cursor advances "far enough". This is
//   essentially a one-shot ring buffer without wraparound — simpler than a
//   true ring buffer, and good enough because HTTP/1.1 requests are bounded.
//
//   For HTTP/1.1 keep-alive we expect requests to be small (<8KB headers).
//   We start with a modest capacity and grow on demand. If a request exceeds
//   max_size_, we'll reject it at the HTTP layer (413 Payload Too Large).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace pulse {

class Buffer {
public:
    static constexpr size_t kInitialCapacity = 4096;
    static constexpr size_t kCompactThreshold = 2048;

    Buffer() { data_.reserve(kInitialCapacity); }

    // Bytes available to read (consume).
    size_t readable() const noexcept { return write_pos_ - read_pos_; }

    // Bytes available to write without growing. Note: capacity may grow on demand.
    size_t writable() const noexcept { return data_.size() - write_pos_; }

    bool empty() const noexcept { return readable() == 0; }

    // Pointer into the read region. Valid until any mutating call.
    const char* read_ptr() const noexcept { return data_.data() + read_pos_; }
    char* write_ptr() noexcept { return data_.data() + write_pos_; }

    std::string_view readable_view() const noexcept {
        return {read_ptr(), readable()};
    }

    // Reserve at least `n` writable bytes. May invalidate previous pointers.
    void ensure_writable(size_t n) {
        if (writable() >= n) return;

        // First try to reclaim space by compacting.
        if (read_pos_ >= kCompactThreshold) {
            compact();
            if (writable() >= n) return;
        }

        // Still not enough — grow. Use vector::resize so capacity is honored.
        // Note: data_.size() is our "logical capacity" here; we resize, not reserve,
        // because we need addressable bytes for raw read(2) into write_ptr().
        size_t needed = write_pos_ + n;
        // Geometric growth to keep amortized O(1) append cost.
        size_t new_size = data_.size() ? data_.size() : kInitialCapacity;
        while (new_size < needed) new_size *= 2;
        data_.resize(new_size);
    }

    // Mark `n` bytes as written (after a read(2) into write_ptr()).
    void commit(size_t n) noexcept { write_pos_ += n; }

    // Mark `n` bytes as consumed (after handing them to send(2) or a parser).
    void consume(size_t n) noexcept {
        read_pos_ += n;
        if (read_pos_ == write_pos_) {
            // Fully drained — reset cursors to keep things tidy and avoid drift.
            read_pos_ = write_pos_ = 0;
        }
    }

    // Append raw bytes. Mostly used by the write path / response builder.
    void append(std::string_view s) {
        ensure_writable(s.size());
        std::memcpy(write_ptr(), s.data(), s.size());
        commit(s.size());
    }

    void append(const char* p, size_t n) {
        ensure_writable(n);
        std::memcpy(write_ptr(), p, n);
        commit(n);
    }

    void clear() noexcept { read_pos_ = write_pos_ = 0; }

private:
    void compact() noexcept {
        size_t n = readable();
        if (n > 0 && read_pos_ > 0) {
            std::memmove(data_.data(), data_.data() + read_pos_, n);
        }
        read_pos_ = 0;
        write_pos_ = n;
    }

    std::vector<char> data_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
};

} // namespace pulse
