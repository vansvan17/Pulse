// pulse/timer_queue.hpp — Min-heap of timeouts for idle-connection eviction.
//
// Why min-heap, not timer wheel:
//   Timer wheels excel when you have millions of pending timers OR very fine
//   granularity (microseconds). We have at most ~one timer per active
//   connection, expiration is seconds-scale, and we mostly need:
//     - schedule O(log n)
//     - peek-soonest O(1)
//     - pop-expired O(log n)
//
//   A min-heap delivers all three with ~30 lines of code. A timer wheel adds
//   bucket arrays, cascading, and complexity. Use the simpler tool until it
//   demonstrably fails to scale.
//
// Why lazy deletion via generation counter:
//   When a connection becomes active, we need to "reset" its timer. Doing
//   that with a decrease-key on a std::priority_queue is impossible (no
//   such operation). The standard trick: each timer entry has a generation
//   number. Resetting the timer increments the connection's generation and
//   schedules a new entry. When we pop an entry whose generation no longer
//   matches the connection's current generation, we discard it.
//
//   This trades memory (stale entries linger) for simplicity and speed.
//   In practice stale entries are bounded by request volume × keep-alive
//   ratio, and get GC'd as the heap is drained.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

namespace pulse {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration  = std::chrono::milliseconds;

inline TimePoint now_steady() { return std::chrono::steady_clock::now(); }

class TimerQueue {
public:
    using ConnId = uint64_t;
    using Generation = uint64_t;

    struct Entry {
        TimePoint deadline;
        ConnId conn_id;
        Generation gen;

        // Min-heap: earliest deadline at the top.
        bool operator<(const Entry& other) const { return deadline > other.deadline; }
    };

    void schedule(ConnId id, Generation gen, TimePoint deadline) {
        heap_.push(Entry{deadline, id, gen});
    }

    // Return all expired (id, gen) pairs at time `now`. Caller validates the
    // generation against the connection's current generation and ignores stale
    // entries. Returns up to `max_pop` items to bound per-tick work.
    template <typename F>
    void pop_expired(TimePoint now, F&& on_expired, size_t max_pop = 256) {
        while (!heap_.empty() && heap_.top().deadline <= now && max_pop-- > 0) {
            Entry e = heap_.top();
            heap_.pop();
            on_expired(e.conn_id, e.gen);
        }
    }

    // Time until next deadline, in ms, capped at `cap`. Used as epoll_wait timeout.
    int next_timeout_ms(TimePoint now, int cap = 1000) const {
        if (heap_.empty()) return cap;
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                        heap_.top().deadline - now).count();
        if (diff < 0) return 0;
        if (diff > cap) return cap;
        return static_cast<int>(diff);
    }

    size_t size() const { return heap_.size(); }

private:
    std::priority_queue<Entry> heap_;
};

} // namespace pulse
