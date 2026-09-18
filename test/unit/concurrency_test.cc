#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "src/core/conflating_slot.h"
#include "src/core/spsc_ring.h"

namespace md {
namespace {

TEST(SpscRing, ReportsFullRatherThanDropping) {
  SpscRing<int> ring(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.Push(int{i}));
  // The queue must refuse rather than overwrite: what flows through it are
  // deltas, and a silent drop corrupts the book permanently.
  EXPECT_FALSE(ring.Push(99));
  int out = -1;
  ASSERT_TRUE(ring.Pop(&out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(ring.Push(99));
}

TEST(SpscRing, PreservesOrderAcrossThreads) {
  constexpr int kCount = 200000;
  SpscRing<int> ring(1024);
  std::vector<int> received;
  received.reserve(kCount);

  std::thread consumer([&] {
    int value = 0;
    while (static_cast<int>(received.size()) < kCount) {
      if (ring.Pop(&value)) received.push_back(value);
    }
  });

  for (int i = 0; i < kCount;) {
    if (ring.Push(int{i})) ++i;
  }
  consumer.join();

  ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) ASSERT_EQ(received[i], i);
}

TEST(ConflatingSlot, KeepsOnlyNewestValue) {
  ConflatingSlot<int> slot;
  slot.Publish(std::make_shared<const int>(1));
  slot.Publish(std::make_shared<const int>(2));
  slot.Publish(std::make_shared<const int>(3));
  EXPECT_EQ(slot.conflated(), 2u);
  auto value = slot.WaitNext();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 3);
}

TEST(ConflatingSlot, StopUnblocksWaiter) {
  ConflatingSlot<int> slot;
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    auto value = slot.WaitNext();
    EXPECT_EQ(value, nullptr);
    returned = true;
  });
  slot.Stop();
  waiter.join();
  EXPECT_TRUE(returned.load());
}

TEST(ConflatingSlot, StopDrainsPendingValueFirst) {
  ConflatingSlot<int> slot;
  slot.Publish(std::make_shared<const int>(7));
  slot.Stop();
  auto value = slot.WaitNext();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 7);
  EXPECT_EQ(slot.WaitNext(), nullptr);
}

// WaitNextFor is the entire fix for the handler-thread leak: the synchronous
// gRPC server cannot interrupt a thread parked in user code, so without a
// bounded wait a subscriber that disconnects while the book is quiet parks a
// handler forever and connect/disconnect churn exhausts the pool.
TEST(ConflatingSlot, WaitNextForTimesOutWithoutAValue) {
  ConflatingSlot<int> slot;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(slot.WaitNextFor(std::chrono::milliseconds(80)), nullptr);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 60);
}

TEST(ConflatingSlot, WaitNextForReturnsImmediatelyWhenValuePending) {
  ConflatingSlot<int> slot;
  slot.Publish(std::make_shared<const int>(42));
  const auto start = std::chrono::steady_clock::now();
  auto value = slot.WaitNextFor(std::chrono::seconds(5));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 42);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
}

TEST(ConflatingSlot, WaitNextForIsUnblockedByStop) {
  ConflatingSlot<int> slot;
  std::thread stopper([&slot] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    slot.Stop();
  });
  EXPECT_EQ(slot.WaitNextFor(std::chrono::seconds(10)), nullptr);
  stopper.join();
}

TEST(ConflatingSlot, PublishNeverBlocksOnSlowConsumer) {
  ConflatingSlot<int> slot;
  std::atomic<bool> stop{false};
  std::atomic<int> seen{0};

  std::thread consumer([&] {
    while (!stop.load()) {
      auto value = slot.WaitNext();
      if (value == nullptr) break;
      seen.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  // A fast writer against a deliberately slow reader. The point is that this
  // loop completes promptly; if Publish blocked, it would not.
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20000; ++i) slot.Publish(std::make_shared<const int>(i));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  stop = true;
  slot.Stop();
  consumer.join();

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
  EXPECT_GT(slot.conflated(), 0u);
  EXPECT_LT(seen.load(), 20000);  // the reader necessarily missed states
}

}  // namespace
}  // namespace md
