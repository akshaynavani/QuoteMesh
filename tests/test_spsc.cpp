#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "quotemesh/spsc_queue.hpp"

using qm::SpscQueue;

TEST(SpscQueue, RoundsCapacityToPowerOfTwo) {
  SpscQueue<int> q(100);
  EXPECT_EQ(q.capacity(), 128u);
}

TEST(SpscQueue, PushPopAndFull) {
  SpscQueue<int> q(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));
  int v = -1;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, BatchPop) {
  SpscQueue<int> q(16);
  for (int i = 0; i < 10; ++i) q.try_push(i);
  int out[16];
  EXPECT_EQ(q.pop_batch(out, 4), 4u);
  EXPECT_EQ(out[3], 3);
  EXPECT_EQ(q.pop_batch(out, 16), 6u);
  EXPECT_EQ(out[0], 4);
  EXPECT_EQ(q.pop_batch(out, 16), 0u);
}

TEST(SpscQueue, ConcurrentProducerConsumerPreservesOrder) {
  constexpr std::uint64_t kCount = 2'000'000;
  SpscQueue<std::uint64_t> q(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kCount; ++i) {
      while (!q.try_push(i)) std::this_thread::yield();
    }
  });
  std::uint64_t expected = 1;
  std::uint64_t buf[64];
  while (expected <= kCount) {
    const auto n = q.pop_batch(buf, 64);
    for (std::size_t i = 0; i < n; ++i) ASSERT_EQ(buf[i], expected++);
  }
  producer.join();
}
