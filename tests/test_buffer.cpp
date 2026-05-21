#include "pulse/buffer.hpp"
#include "test_harness.hpp"

#include <cstring>
#include <string>

using namespace pulse;
using namespace pulse::test;

int main() {
    {
        start("empty_buffer");
        Buffer b;
        EXPECT_EQ(b.readable(), 0u);
        EXPECT_TRUE(b.empty());
        finish();
    }
    {
        start("append_and_read");
        Buffer b;
        b.append("hello");
        EXPECT_EQ(b.readable(), 5u);
        EXPECT_STREQ(std::string(b.readable_view()), "hello");
        finish();
    }
    {
        start("consume_then_append_compacts");
        Buffer b;
        // Fill with > kCompactThreshold bytes, consume past threshold, append
        // should trigger compaction.
        std::string big(3000, 'a');
        b.append(big);
        b.consume(2500);   // read_pos = 2500 > kCompactThreshold
        EXPECT_EQ(b.readable(), 500u);
        b.append("X");
        EXPECT_EQ(b.readable(), 501u);
        EXPECT_EQ(b.readable_view().back(), 'X');
        finish();
    }
    {
        start("ensure_writable_grows");
        Buffer b;
        size_t before = b.writable();
        b.ensure_writable(before + 100);
        EXPECT_TRUE(b.writable() >= before + 100);
        finish();
    }
    {
        start("commit_after_raw_write");
        Buffer b;
        b.ensure_writable(16);
        std::memcpy(b.write_ptr(), "0123456789", 10);
        b.commit(10);
        EXPECT_EQ(b.readable(), 10u);
        EXPECT_STREQ(std::string(b.readable_view()), "0123456789");
        finish();
    }
    {
        start("full_consume_resets");
        Buffer b;
        b.append("xyz");
        b.consume(3);
        EXPECT_EQ(b.readable(), 0u);
        // After full consume, internal cursors should reset so writable is max.
        // We test indirectly: appending again should still work.
        b.append("abc");
        EXPECT_STREQ(std::string(b.readable_view()), "abc");
        finish();
    }
    return summary();
}
