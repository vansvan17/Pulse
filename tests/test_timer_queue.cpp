#include "pulse/timer_queue.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <vector>

using namespace pulse;
using namespace pulse::test;

int main() {
    using namespace std::chrono;
    {
        start("empty_no_expirations");
        TimerQueue q;
        int fired = 0;
        q.pop_expired(now_steady(), [&](uint64_t, uint64_t) { ++fired; });
        EXPECT_EQ(fired, 0);
        finish();
    }
    {
        start("expires_in_deadline_order");
        TimerQueue q;
        auto base = now_steady();
        q.schedule(1, 1, base + milliseconds(300));
        q.schedule(2, 1, base + milliseconds(100));
        q.schedule(3, 1, base + milliseconds(200));

        std::vector<uint64_t> order;
        q.pop_expired(base + milliseconds(500), [&](uint64_t id, uint64_t) {
            order.push_back(id);
        });
        EXPECT_EQ(order.size(), 3u);
        EXPECT_EQ(order[0], 2u);
        EXPECT_EQ(order[1], 3u);
        EXPECT_EQ(order[2], 1u);
        finish();
    }
    {
        start("not_yet_expired");
        TimerQueue q;
        auto base = now_steady();
        q.schedule(1, 1, base + seconds(10));
        int fired = 0;
        q.pop_expired(base, [&](uint64_t, uint64_t) { ++fired; });
        EXPECT_EQ(fired, 0);
        finish();
    }
    {
        start("lazy_deletion_stale_generation");
        // Schedule timer with gen=1, then user "touches" the connection making
        // gen=2 and re-schedules. The gen=1 entry is now stale. When it fires,
        // the on_expired callback sees gen=1 and the caller (loop) discards.
        TimerQueue q;
        auto base = now_steady();
        q.schedule(42, 1, base + milliseconds(50));   // stale-to-be
        q.schedule(42, 2, base + milliseconds(100));  // fresh

        std::vector<uint64_t> gens;
        q.pop_expired(base + milliseconds(200), [&](uint64_t, uint64_t gen) {
            gens.push_back(gen);
        });
        EXPECT_EQ(gens.size(), 2u);
        EXPECT_EQ(gens[0], 1u);   // stale fires first (earlier deadline)
        EXPECT_EQ(gens[1], 2u);
        finish();
    }
    {
        start("next_timeout_ms_clamps");
        TimerQueue q;
        auto base = now_steady();
        q.schedule(1, 1, base + seconds(5));
        int ms = q.next_timeout_ms(base, 1000);
        EXPECT_TRUE(ms == 1000);   // clamped to cap
        finish();
    }
    return summary();
}
