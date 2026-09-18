#include "src/core/retry.h"

#include <gtest/gtest.h>

namespace md {
namespace {

using ms = std::chrono::milliseconds;

// REGRESSION. This shipped once with the reset in the wrong place, which pinned
// every reconnect at the initial delay and made the maximum dead code -- so a
// venue that was down, rate-limited or already returning 418 got retried twice a
// second indefinitely. It is pure arithmetic over one member and a ten-line test
// would have caught it, which is why the test exists now.
TEST(ExponentialBackoff, GrowsAcrossConsecutiveFailures) {
  ExponentialBackoff backoff(ms(500), ms(30000));
  EXPECT_EQ(backoff.peek(), ms(500));
  backoff.Next();
  EXPECT_EQ(backoff.peek(), ms(1000));
  backoff.Next();
  EXPECT_EQ(backoff.peek(), ms(2000));
  backoff.Next();
  EXPECT_EQ(backoff.peek(), ms(4000));
}

TEST(ExponentialBackoff, SaturatesAtTheMaximum) {
  ExponentialBackoff backoff(ms(500), ms(2000));
  for (int i = 0; i < 20; ++i) backoff.Next();
  EXPECT_EQ(backoff.peek(), ms(2000));
}

TEST(ExponentialBackoff, ResetOnlyHappensWhenAsked) {
  ExponentialBackoff backoff(ms(500), ms(30000));
  backoff.Next();
  backoff.Next();
  ASSERT_EQ(backoff.peek(), ms(2000));
  backoff.Reset();
  EXPECT_EQ(backoff.peek(), ms(500));
}

// Full jitter: the delay is drawn from [window/2, window], never a fixed value,
// so venues that dropped on one network blip do not retry in lockstep.
TEST(ExponentialBackoff, AppliesJitterWithinTheWindow) {
  ExponentialBackoff backoff(ms(1000), ms(30000));
  const ms delay = backoff.Next();
  EXPECT_GE(delay, ms(500));
  EXPECT_LE(delay, ms(1000));
}

// This budget is what stands between a resync loop and a Binance IP ban.
TEST(RateBudget, EnforcesTheLimitWithinTheWindow) {
  RateBudget budget(3, std::chrono::minutes(1));
  const auto now = std::chrono::steady_clock::now();
  EXPECT_TRUE(budget.TryConsume(now));
  EXPECT_TRUE(budget.TryConsume(now));
  EXPECT_TRUE(budget.TryConsume(now));
  EXPECT_FALSE(budget.TryConsume(now));
  EXPECT_EQ(budget.used(), 3);
}

TEST(RateBudget, WindowSlidesRatherThanResetting) {
  RateBudget budget(2, std::chrono::minutes(1));
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(budget.TryConsume(start));
  ASSERT_TRUE(budget.TryConsume(start + std::chrono::seconds(30)));
  EXPECT_FALSE(budget.TryConsume(start + std::chrono::seconds(31)));

  // Only the first event has aged out at t+61s, so exactly one slot frees up.
  EXPECT_TRUE(budget.TryConsume(start + std::chrono::seconds(61)));
  EXPECT_FALSE(budget.TryConsume(start + std::chrono::seconds(62)));
}

}  // namespace
}  // namespace md
