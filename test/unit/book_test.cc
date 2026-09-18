#include "src/core/book.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace md {
namespace {

std::vector<Px> Prices(const BidBook& book) {
  std::vector<Px> out;
  for (const Level& level : book.levels()) out.push_back(level.px);
  return out;
}

TEST(SideBook, BidsSortDescendingAsksAscending) {
  BidBook bids;
  bids.Apply(100, 1);
  bids.Apply(102, 1);
  bids.Apply(101, 1);
  EXPECT_EQ(Prices(bids), (std::vector<Px>{102, 101, 100}));
  EXPECT_EQ(bids.BestPx(), 102);

  AskBook asks;
  asks.Apply(100, 1);
  asks.Apply(102, 1);
  asks.Apply(101, 1);
  EXPECT_EQ(asks.BestPx(), 100);
  EXPECT_TRUE(asks.Invariant());
}

TEST(SideBook, ZeroQuantityRemovesLevel) {
  BidBook bids;
  bids.Apply(100, 5);
  bids.Apply(99, 5);
  bids.Apply(100, 0);
  EXPECT_EQ(Prices(bids), (std::vector<Px>{99}));
  // Removing something absent is a no-op, not an insert of a zero level.
  bids.Apply(50, 0);
  EXPECT_EQ(bids.size(), 1u);
  EXPECT_TRUE(bids.Invariant());
}

TEST(SideBook, ApplyOverwritesQuantity) {
  BidBook bids;
  bids.Apply(100, 5);
  bids.Apply(100, 7);
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids.BestQty(), 7);
}

// The batched path and the one-at-a-time path must be indistinguishable. This
// matters because the batch path only engages above a size threshold, so a bug
// in it would hide on small frames and appear only under load.
TEST(SideBook, BatchApplyMatchesIndividualApply) {
  std::mt19937 rng(20260917);
  std::uniform_int_distribution<int> price_dist(1, 400);
  std::uniform_int_distribution<int> qty_dist(0, 5);

  for (int trial = 0; trial < 200; ++trial) {
    BidBook incremental;
    BidBook batched;
    for (Px px = 1; px <= 300; ++px) {
      incremental.Apply(px, 10);
      batched.Apply(px, 10);
    }

    std::vector<Level> deltas;
    const int count = 1 + (trial % 40);
    for (int i = 0; i < count; ++i) {
      deltas.push_back(Level{price_dist(rng), qty_dist(rng)});
    }

    for (const Level& level : deltas) incremental.Apply(level.px, level.qty);
    std::vector<Level> copy = deltas;
    batched.ApplyBatch(&copy);

    ASSERT_TRUE(incremental.Invariant()) << "trial " << trial;
    ASSERT_TRUE(batched.Invariant()) << "trial " << trial;
    ASSERT_EQ(incremental.size(), batched.size()) << "trial " << trial;
    for (std::size_t i = 0; i < incremental.size(); ++i) {
      EXPECT_EQ(incremental.levels()[i].px, batched.levels()[i].px);
      EXPECT_EQ(incremental.levels()[i].qty, batched.levels()[i].qty);
    }
  }
}

TEST(SideBook, BatchApplyResolvesDuplicatePricesLastWins) {
  BidBook bids;
  std::vector<Level> deltas{{100, 1}, {101, 1}, {102, 1}, {103, 1},
                            {104, 1}, {105, 1}, {106, 1}, {107, 1},
                            {100, 9}};  // repeated price, later entry wins
  bids.ApplyBatch(&deltas);
  ASSERT_TRUE(bids.Invariant());
  const auto levels = bids.levels();
  EXPECT_EQ(levels.back().px, 100);
  EXPECT_EQ(levels.back().qty, 9);
}

TEST(SideBook, ReplaceInstallsSnapshotAndDropsZeros) {
  BidBook bids;
  bids.Apply(1, 1);
  bids.Replace({{100, 1}, {102, 2}, {101, 0}, {99, 3}});
  EXPECT_EQ(Prices(bids), (std::vector<Px>{102, 100, 99}));
  EXPECT_TRUE(bids.Invariant());
}

TEST(VenueBook, DetectsSelfCross) {
  VenueBook book;
  book.bids.Apply(100, 1);
  book.asks.Apply(101, 1);
  EXPECT_FALSE(book.Crossed());
  // A single venue's own book crossing means our delta application is wrong.
  book.asks.Apply(99, 1);
  EXPECT_TRUE(book.Crossed());
}

}  // namespace
}  // namespace md
