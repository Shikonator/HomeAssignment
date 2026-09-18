#include "src/core/fixed.h"

#include <gtest/gtest.h>

#include <string>

namespace md {
namespace {

TEST(ParseFixed, AcceptsPlainDecimals) {
  std::int64_t v = 0;
  ASSERT_TRUE(ParseFixed("68123.45", &v));
  EXPECT_EQ(v, 6812345000000);
  ASSERT_TRUE(ParseFixed("0", &v));
  EXPECT_EQ(v, 0);
  ASSERT_TRUE(ParseFixed("1", &v));
  EXPECT_EQ(v, kScale);
  ASSERT_TRUE(ParseFixed("0.00000001", &v));
  EXPECT_EQ(v, 1);
}

TEST(ParseFixed, HandlesExchangeFormatting) {
  std::int64_t v = 0;
  // Trailing zeros: how Binance publishes almost everything.
  ASSERT_TRUE(ParseFixed("0.10000000", &v));
  EXPECT_EQ(v, 10000000);
  // Leading zeros and a bare decimal point form.
  ASSERT_TRUE(ParseFixed("007.5", &v));
  EXPECT_EQ(v, 750000000);
  ASSERT_TRUE(ParseFixed(".5", &v));
  EXPECT_EQ(v, 50000000);
  ASSERT_TRUE(ParseFixed("5.", &v));
  EXPECT_EQ(v, 500000000);
}

TEST(ParseFixed, TruncatesBeyondEightDecimals) {
  std::int64_t v = 0;
  ASSERT_TRUE(ParseFixed("1.123456789", &v));
  EXPECT_EQ(v, 112345678);
  ASSERT_TRUE(ParseFixed("1.999999999", &v));
  EXPECT_EQ(v, 199999999);
}

TEST(ParseFixed, RejectsMalformed) {
  std::int64_t v = 0;
  EXPECT_FALSE(ParseFixed("", &v));
  EXPECT_FALSE(ParseFixed("abc", &v));
  EXPECT_FALSE(ParseFixed("1.2.3", &v));
  EXPECT_FALSE(ParseFixed("1e8", &v));       // exponent notation is not accepted
  EXPECT_FALSE(ParseFixed("1.5 ", &v));      // trailing whitespace is garbage
  EXPECT_FALSE(ParseFixed(" 1.5", &v));
  EXPECT_FALSE(ParseFixed("-", &v));
  EXPECT_FALSE(ParseFixed("+", &v));
  EXPECT_FALSE(ParseFixed("99999999999.0", &v));  // integer part overflows
}

TEST(ParseFixed, Negatives) {
  std::int64_t v = 0;
  ASSERT_TRUE(ParseFixed("-1.5", &v));
  EXPECT_EQ(v, -150000000);
}

TEST(FormatFixed, RoundTrips) {
  const char* cases[] = {"0.00", "1.00", "68123.45", "0.00000001", "-1.50", "99.99999999"};
  for (const char* text : cases) {
    std::int64_t v = 0;
    ASSERT_TRUE(ParseFixed(text, &v)) << text;
    std::int64_t again = 0;
    ASSERT_TRUE(ParseFixed(FormatFixed(v), &again)) << text;
    EXPECT_EQ(v, again) << text << " -> " << FormatFixed(v);
  }
}

TEST(FormatFixed, TrimsToMinimumDecimals) {
  EXPECT_EQ(FormatFixed(kScale), "1.00");
  EXPECT_EQ(FormatFixed(kScale, 0), "1");
  EXPECT_EQ(FormatFixed(150000000), "1.50");
  EXPECT_EQ(FormatFixed(1), "0.00000001");
  EXPECT_EQ(FormatFixed(-150000000), "-1.50");
}

// The reason every product in this codebase goes through __int128: a price of
// 100k and a quantity of one hundredth of a coin already overflow int64.
TEST(Notional, DoesNotOverflowAtRealisticSizes) {
  const Px price = 100'000 * kScale;   // 1e13
  const Qty tiny = kScale / 100;       // 0.01 BTC -> 1e6
  EXPECT_GT(static_cast<Wide>(price) * tiny, static_cast<Wide>(INT64_MAX));
  EXPECT_EQ(NotionalE8(price, tiny), 1'000 * kScale);  // exactly 1000 USDT

  // The 50M band on a deep book: 500 BTC at 100k.
  const Qty five_hundred = 500 * kScale;
  EXPECT_EQ(NotionalE8(price, five_hundred), static_cast<Notional>(50'000'000) * kScale);
}

TEST(Notional, QtyForNotionalInvertsCleanly) {
  const Px price = 100'000 * kScale;
  const Wide notional = static_cast<Wide>(50'000'000) * kScale;
  EXPECT_EQ(QtyForNotional(notional, price), 500 * kScale);
}

TEST(Vwap, WeightsBySize) {
  // 1.5 @ 100 and 1.0 @ 99 -> 249 over 2.5 -> 99.6
  const Wide notional = static_cast<Wide>(249) * kScale;
  const Wide qty = static_cast<Wide>(250000000);
  EXPECT_EQ(Vwap(notional, qty), 9960000000);
}

TEST(Bps, OffsetsRoundOutwardOnBothSides) {
  const Px reference = 100 * kScale;
  const std::int64_t one_percent = 100 * kScale;  // 100 bps
  EXPECT_EQ(OffsetByBpsOutward(reference, one_percent, /*downward=*/true), 99 * kScale);
  EXPECT_EQ(OffsetByBpsOutward(reference, one_percent, /*downward=*/false), 101 * kScale);

  // A bound that does not land on an exact unit must move OUTWARD on both
  // sides, never inward, so a level sitting on the boundary is treated the same
  // way for bids and asks.
  const Px odd = 333 * kScale + 33;
  const std::int64_t bps = 7 * kScale;
  EXPECT_LT(OffsetByBpsOutward(odd, bps, true), odd);
  EXPECT_GT(OffsetByBpsOutward(odd, bps, false), odd);
}

TEST(Bps, SpreadInBasisPoints) {
  // 1.00 wide on a 100.00 mid is exactly 100 bps.
  EXPECT_EQ(BpsE8(1 * kScale, 100 * kScale), 100 * kScale);
}

}  // namespace
}  // namespace md
