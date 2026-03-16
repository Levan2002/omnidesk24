#include <gtest/gtest.h>

#include "core/ring_buffer.h"

#include <atomic>
#include <thread>
#include <vector>

namespace omnidesk {

TEST(RingBuffer, PushPopSingleItem) {
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_TRUE(rb.push(42));
    EXPECT_FALSE(rb.empty());

    auto val = rb.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, PushUntilFull) {
    // Capacity 4, but SPSC ring uses one slot as sentinel, so max items = 3.
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.push(3));
    EXPECT_FALSE(rb.push(4));  // Full
}

TEST(RingBuffer, PopFromEmpty) {
    RingBuffer<int, 4> rb;
    auto val = rb.pop();
    EXPECT_FALSE(val.has_value());
}

TEST(RingBuffer, FillAndDrain) {
    RingBuffer<int, 8> rb;

    // Fill (capacity 8, usable slots = 7)
    for (int i = 0; i < 7; ++i) {
        EXPECT_TRUE(rb.push(i + 100));
    }
    EXPECT_FALSE(rb.push(999));  // Full

    // Drain and verify order
    for (int i = 0; i < 7; ++i) {
        auto val = rb.pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i + 100);
    }
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.pop().has_value());
}

TEST(RingBuffer, MultiThreadedProducerConsumer) {
    constexpr int kNumItems = 10000;
    RingBuffer<int, 1024> rb;

    std::atomic<bool> done{false};
    std::vector<int> received;
    received.reserve(kNumItems);

    // Consumer thread
    std::thread consumer([&] {
        while (true) {
            auto val = rb.pop();
            if (val.has_value()) {
                received.push_back(*val);
                if (received.size() == kNumItems) break;
            } else if (done.load(std::memory_order_acquire)) {
                // Drain remaining
                while (auto v = rb.pop()) {
                    received.push_back(*v);
                }
                break;
            }
        }
    });

    // Producer thread
    std::thread producer([&] {
        for (int i = 0; i < kNumItems; ++i) {
            while (!rb.push(i)) {
                // Spin until space available
            }
        }
        done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kNumItems));
    for (int i = 0; i < kNumItems; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

} // namespace omnidesk
