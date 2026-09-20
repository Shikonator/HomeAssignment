#include "src/core/analytics.h"

#include <gtest/gtest.h>

#include <vector>

#include "src/core/consolidated.h"

namespace md {
namespace {

constexpr Px P(double units) { return static_cast<Px>(units * kScale); }

// Fixture used throughout, with every expected number computed by hand:
//
//   venue 0 bids: 100.00 x 1.0   99.00 x 2.0
//   venue 1 bids: 100.00 x 0.5                 98.00 x 3.0
//
//   merged:       100.00 x 1.5   99.00 x 2.0   98.00 x 3.0
//   level value:  150.00         198.00        294.00
//   cumulative:   150.00         348.00        642.00
std::vector<MergedLevel> MakeBidLadder() {
  std::vector<MergedLevel> ladder(3);
  ladder[0].px = P(100);
  ladder[0].qty = P(1.5);
  ladder[1].px = P(99);
  ladder[1].qty = P(2.0);
  ladder[2].px = P(98);
  ladder[2].qty = P(3.0);
  return ladder;
}

LadderView BidView(const std::vector<MergedLevel>& ladder) {
  return LadderView{ladder, true};
}

BandConfig Config() {
  BandConfig config;
  config.notionals_e8 = {P(150), P(249), P(1000)};
  config.offsets_bps_e8 = {50 * kScale, 100 * kScale, 1000 * kScale};
  return config;
}

TEST(VolumeBands, FillsExactlyAtALevelBoundary) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  ASSERT_EQ(bands.volume.size(), 4u);  // three targets plus the open-ended band
  const VolumeBandResult& band = bands.volume[0];
  EXPECT_EQ(band.target_e8, P(150));
  EXPECT_EQ(band.filled_qty_e8, P(1.5));
  EXPECT_EQ(band.filled_notional_e8, P(150));
  EXPECT_EQ(band.vwap_e8, P(100));
  EXPECT_EQ(band.worst_e8, P(100));
  EXPECT_TRUE(band.fully_filled);
  EXPECT_EQ(band.levels_consumed, 1);
}

// 1.5 @ 100 = 150, then 1.0 of the 2.0 available @ 99 = 99. 249 over 2.5 is a
// VWAP of exactly 99.60. Consuming only part of the boundary level is the
// normal case and is what makes the VWAP correct.
TEST(VolumeBands, ConsumesBoundaryLevelPartially) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  const VolumeBandResult& band = bands.volume[1];
  EXPECT_EQ(band.target_e8, P(249));
  EXPECT_EQ(band.filled_qty_e8, P(2.5));
  EXPECT_EQ(band.filled_notional_e8, P(249));
  EXPECT_EQ(band.vwap_e8, P(99.6));
  EXPECT_EQ(band.worst_e8, P(99));
  EXPECT_TRUE(band.fully_filled);
  EXPECT_EQ(band.levels_consumed, 2);
}

TEST(VolumeBands, ReportsPartialSweepWhenBookIsTooThin) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  const VolumeBandResult& band = bands.volume[2];
  EXPECT_EQ(band.target_e8, P(1000));
  EXPECT_FALSE(band.fully_filled);
  EXPECT_EQ(band.filled_qty_e8, P(6.5));
  EXPECT_EQ(band.filled_notional_e8, P(642));
  EXPECT_EQ(band.vwap_e8, 9876923076);  // 642 / 6.5, truncated toward zero
  EXPECT_EQ(band.worst_e8, P(98));
  EXPECT_EQ(band.levels_consumed, 3);
}

// The specification asks for "50M+", i.e. a trailing unbounded band.
TEST(VolumeBands, EmitsTrailingOpenEndedBand) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  const VolumeBandResult& band = bands.volume.back();
  EXPECT_TRUE(band.open_ended);
  EXPECT_EQ(band.target_e8, 0);       // no target: the field stays a unique key
  EXPECT_FALSE(band.fully_filled);    // "met the target" is undefined here
  EXPECT_EQ(band.filled_qty_e8, P(6.5));
  EXPECT_EQ(band.filled_notional_e8, P(642));
}

TEST(PriceBands, AccumulateCumulativelyFromTheTouch) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  ASSERT_EQ(bands.price.size(), 4u);

  // 50 bps below 100.00 is 99.50: only the touch qualifies.
  EXPECT_EQ(bands.price[0].bound_e8, P(99.5));
  EXPECT_EQ(bands.price[0].qty_e8, P(1.5));
  EXPECT_EQ(bands.price[0].notional_e8, P(150));
  EXPECT_EQ(bands.price[0].vwap_e8, P(100));
  EXPECT_EQ(bands.price[0].levels, 1);
  EXPECT_FALSE(bands.price[0].depth_limited);

  // 100 bps is exactly 99.00, and the bound is inclusive.
  EXPECT_EQ(bands.price[1].bound_e8, P(99));
  EXPECT_EQ(bands.price[1].qty_e8, P(3.5));
  EXPECT_EQ(bands.price[1].notional_e8, P(348));
  EXPECT_EQ(bands.price[1].vwap_e8, 9942857142);  // 348 / 3.5
  EXPECT_EQ(bands.price[1].levels, 2);
  EXPECT_FALSE(bands.price[1].depth_limited);
}

// 1000 bps below 100.00 is 90.00, far past the end of this ladder. The band is
// still emitted, reporting all available depth, and flagged so a consumer is
// never told "liquidity within 1000bps" when it is really "everything we have".
TEST(PriceBands, FlagsBandsThatOutrunThePublishedLadder) {
  const auto ladder = MakeBidLadder();
  SideBands bands;
  ComputeSideBands(BidView(ladder), Config(), &bands);

  EXPECT_EQ(bands.price[2].bound_e8, P(90));
  EXPECT_EQ(bands.price[2].qty_e8, P(6.5));
  EXPECT_EQ(bands.price[2].levels, 3);
  EXPECT_TRUE(bands.price[2].depth_limited);

  const PriceBandResult& open = bands.price.back();
  EXPECT_TRUE(open.open_ended);
  EXPECT_EQ(open.bound_e8, P(98));  // the worst price actually present
  EXPECT_EQ(open.qty_e8, P(6.5));
  EXPECT_EQ(open.notional_e8, P(642));
}

TEST(Touch, NormalBook) {
  std::vector<MergedLevel> bids(1), asks(1);
  bids[0].px = P(100);
  bids[0].qty = P(1);
  asks[0].px = P(101);
  asks[0].qty = P(1);

  LadderView bid_view{bids, true};
  LadderView ask_view{asks, false};
  const TouchInfo touch = ComputeTouch(bid_view, ask_view);

  EXPECT_TRUE(touch.has_bid);
  EXPECT_TRUE(touch.has_ask);
  EXPECT_EQ(touch.mid, P(100.5));
  EXPECT_EQ(touch.spread_e8, P(1));
  EXPECT_EQ(touch.spread_bps_e8, 9950248756);  // 1.00 / 100.50
  EXPECT_FALSE(touch.crossed);
}

// Independent venues publish at different cadences and latencies, so the
// consolidated touch crosses routinely. It is reported, not clamped.
TEST(Touch, CrossedBookIsReportedWithSignedSpread) {
  std::vector<MergedLevel> bids(1), asks(1);
  bids[0].px = P(101);
  bids[0].qty = P(1);
  asks[0].px = P(100);
  asks[0].qty = P(1);

  LadderView bid_view{bids, true};
  LadderView ask_view{asks, false};
  const TouchInfo touch = ComputeTouch(bid_view, ask_view);

  EXPECT_TRUE(touch.crossed);
  EXPECT_EQ(touch.spread_e8, P(-1));
  EXPECT_LT(touch.spread_bps_e8, 0);
  EXPECT_EQ(touch.mid, P(100.5));
}

// Reachable before the first venue syncs, when every venue is stale, and in
// single-venue filtered views.
TEST(Touch, EmptySideYieldsZeroedDerivedFields) {
  std::vector<MergedLevel> bids(1), empty;
  bids[0].px = P(100);
  bids[0].qty = P(1);

  LadderView bid_view{bids, true};
  LadderView ask_view{empty, false};
  const TouchInfo touch = ComputeTouch(bid_view, ask_view);

  EXPECT_TRUE(touch.has_bid);
  EXPECT_FALSE(touch.has_ask);
  EXPECT_EQ(touch.best_ask, 0);
  EXPECT_EQ(touch.mid, 0);
  EXPECT_EQ(touch.spread_e8, 0);
  EXPECT_EQ(touch.spread_bps_e8, 0);
  EXPECT_FALSE(touch.crossed);
}

TEST(Bands, EmptySideStillEmitsAlignedBands) {
  std::vector<MergedLevel> empty;
  LadderView view{empty, true};
  SideBands bands;
  ComputeSideBands(view, Config(), &bands);

  ASSERT_EQ(bands.volume.size(), 4u);
  ASSERT_EQ(bands.price.size(), 4u);
  EXPECT_EQ(bands.volume[0].target_e8, P(150));
  EXPECT_EQ(bands.volume[0].filled_qty_e8, 0);
  EXPECT_FALSE(bands.volume[0].fully_filled);
  EXPECT_TRUE(bands.price[0].depth_limited);
}

TEST(MergeSide, InterleavesVenuesAndSumsSharedPrices) {
  const std::vector<Level> venue0{{P(100), P(1.0)}, {P(99), P(2.0)}};
  const std::vector<Level> venue1{{P(100), P(0.5)}, {P(98), P(3.0)}};
  const std::vector<VenueSideInput> inputs{{0, venue0}, {1, venue1}};

  std::vector<MergedLevel> out;
  MergeLimits limits;
  const bool truncated = MergeSide(true, inputs, limits, &out);

  EXPECT_FALSE(truncated);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].px, P(100));
  EXPECT_EQ(out[0].qty, P(1.5));
  EXPECT_EQ(out[1].px, P(99));
  EXPECT_EQ(out[2].px, P(98));
}

TEST(MergeSide, StopsAtNotionalTargetAndReportsTruncation) {
  const std::vector<Level> venue0{{P(100), P(1.0)}, {P(99), P(2.0)}, {P(98), P(3.0)}};
  const std::vector<VenueSideInput> inputs{{0, venue0}};

  std::vector<MergedLevel> out;
  MergeLimits limits;
  limits.notional_target_e8 = P(150);  // reached partway through the ladder
  EXPECT_TRUE(MergeSide(true, inputs, limits, &out));
  EXPECT_EQ(out.size(), 2u);
}

}  // namespace
}  // namespace md
